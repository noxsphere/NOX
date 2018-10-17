// Copyright (c) 2018, The TurtleCoin Developers
// 
// Please see the included LICENSE file for more information.

////////////////////////////////////////
#include <WalletBackend/WalletBackend.h>
////////////////////////////////////////

#include <config/CryptoNoteConfig.h>

#include <CryptoNoteCore/Account.h>
#include <CryptoNoteCore/CryptoNoteBasicImpl.h>

#include <cryptopp/aes.h>
#include <cryptopp/algparam.h>
#include <cryptopp/filters.h>
#include <cryptopp/modes.h>
#include <cryptopp/sha.h>
#include <cryptopp/pwdbased.h>

#include <fstream>
#include <future>

#include "json.hpp"

#include <Mnemonics/Mnemonics.h>

#include <WalletBackend/Constants.h>
#include <WalletBackend/JsonSerialization.h>
#include <WalletBackend/Utilities.h>
#include <WalletBackend/ValidateParameters.h>

using json = nlohmann::json;

//////////////////////////
/* NON MEMBER FUNCTIONS */
//////////////////////////

/* Anonymous namespace so it doesn't clash with anything else */
namespace {

/* Check data has the magic indicator from first : last, and remove it if
   it does. Else, return an error depending on where we failed */
template <class Iterator, class Buffer>
WalletError hasMagicIdentifier(Buffer &data, Iterator first, Iterator last,
                               WalletError tooSmallError,
                               WalletError wrongIdentifierError)
{
    size_t identifierSize = std::distance(first, last);

    /* Check we've got space for the identifier */
    if (data.size() < identifierSize)
    {
        return tooSmallError;
    }

    for (size_t i = 0; i < identifierSize; i++)
    {
        if ((int)data[i] != *(first + i))
        {
            return wrongIdentifierError;
        }
    }

    /* Remove the identifier from the string */
    data.erase(data.begin(), data.begin() + identifierSize);

    return SUCCESS;
}

/* Generates a public address from the given private keys */
std::string addressFromPrivateKeys(const Crypto::SecretKey &privateSpendKey,
                                   const Crypto::SecretKey &privateViewKey)
{
    Crypto::PublicKey publicSpendKey;
    Crypto::PublicKey publicViewKey;

    Crypto::secret_key_to_public_key(privateSpendKey, publicSpendKey);
    Crypto::secret_key_to_public_key(privateViewKey, publicViewKey);

    return CryptoNote::getAccountAddressAsStr(
        CryptoNote::parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX,
        { publicSpendKey, publicViewKey }
    );
}

/* Check the wallet filename for the new wallet to be created is valid */
WalletError checkNewWalletFilename(std::string filename)
{
    /* Check the file doesn't exist */
    if (std::ifstream(filename))
    {
        return WALLET_FILE_ALREADY_EXISTS;
    }

    /* Check we can open the file */
    if (!std::ofstream(filename))
    {
        return INVALID_WALLET_FILENAME;
    }
    
    return SUCCESS;
}

} // namespace

///////////////////////////////////
/* CONSTRUCTORS / DECONSTRUCTORS */
///////////////////////////////////

/* Constructor */
WalletBackend::WalletBackend()
{
    m_logManager = std::make_shared<Logging::LoggerManager>();

    m_logger = std::make_shared<Logging::LoggerRef>(
        *m_logManager, "WalletBackend"
    );

    /* Remember to call initializeAfterLoad() to initialize the daemon - 
    we can't do it here since we don't have the host/port, and the json
    serialization uses the default constructor */
}

/* Deconstructor */
WalletBackend::~WalletBackend()
{
    /* Save, but only if the non default constructor was used - else things
       will be uninitialized, and crash */
    if (m_daemon != nullptr)
    {
        save();
    }
}

/* Move constructor */
WalletBackend::WalletBackend(WalletBackend && old)
{
    /* Call the move assignment operator */
    *this = std::move(old);
}

/* Move assignment operator */
WalletBackend & WalletBackend::operator=(WalletBackend && old)
{
    m_filename = old.m_filename;
    m_password = old.m_password;
    m_privateViewKey = old.m_privateViewKey;
    m_isViewWallet = old.m_isViewWallet;
    m_logManager = old.m_logManager;
    m_daemon = old.m_daemon;
    m_subWallets = old.m_subWallets;
    m_walletSynchronizer = old.m_walletSynchronizer;
    m_eventHandler = old.m_eventHandler;

    /* Invalidate the old pointers */
    old.m_logManager = nullptr;
    old.m_daemon = nullptr;
    old.m_logger = nullptr;
    old.m_subWallets = nullptr;
    old.m_walletSynchronizer = nullptr;
    old.m_eventHandler = nullptr;

    return *this;
}

/* Constructor */
WalletBackend::WalletBackend(std::string filename, std::string password,
                             Crypto::SecretKey privateSpendKey,
                             Crypto::SecretKey privateViewKey,
                             bool isViewWallet, uint64_t scanHeight,
                             bool newWallet, std::string daemonHost,
                             uint16_t daemonPort) :
    m_filename(filename),
    m_password(password),
    m_privateViewKey(privateViewKey),
    m_isViewWallet(isViewWallet)
{
    m_logManager = std::make_shared<Logging::LoggerManager>();

    m_logger = std::make_shared<Logging::LoggerRef>(
        *m_logManager, "WalletBackend"
    );

    m_daemon = std::make_shared<CryptoNote::NodeRpcProxy>(
        daemonHost, daemonPort, m_logger->getLogger()
    );

    /* Generate the address from the two private keys */
    std::string address = addressFromPrivateKeys(privateSpendKey,
                                                 privateViewKey);

    m_subWallets = std::make_shared<SubWallets>(
        privateSpendKey, address, scanHeight, newWallet
    );

    m_eventHandler = std::make_shared<EventHandler>();
}

/////////////////////
/* CLASS FUNCTIONS */
/////////////////////

/* Imports a wallet from a mnemonic seed. Returns the wallet class,
   or an error. */
std::tuple<WalletError, WalletBackend> WalletBackend::importWalletFromSeed(
    const std::string mnemonicSeed, const std::string filename,
    const std::string password, const uint64_t scanHeight,
    const std::string daemonHost, const uint16_t daemonPort)
{
    /* Check the filename is valid */
    if (WalletError error = checkNewWalletFilename(filename); error != SUCCESS)
    {
        return {error, WalletBackend()};
    }

    /* Convert the mnemonic into a private spend key */
    auto [mnemonicError, privateSpendKey] = Mnemonics::MnemonicToPrivateKey(mnemonicSeed);

    /* TODO: Return a more informative error */
    if (!mnemonicError.empty())
    {
        return {INVALID_MNEMONIC, WalletBackend()};
    }

    Crypto::SecretKey privateViewKey;

    /* Derive the private view key from the private spend key */
    CryptoNote::AccountBase::generateViewFromSpend(privateSpendKey,
                                                   privateViewKey);

    /* Just defining here so it's more obvious what we're doing in the
       constructor */
    bool newWallet = false;
    bool isViewWallet = false;

    WalletBackend wallet(
        filename, password, privateSpendKey, privateViewKey, isViewWallet,
        scanHeight, newWallet, daemonHost, daemonPort
    );

    if (WalletError error = wallet.init(); error != SUCCESS)
    {
        return {error, WalletBackend()};
    }

    /* Save to disk */
    WalletError error = wallet.save();

    return {error, std::move(wallet)};
}

/* Imports a wallet from a private spend key and a view key. Returns
   the wallet class, or an error. */
std::tuple<WalletError, WalletBackend> WalletBackend::importWalletFromKeys(
    Crypto::SecretKey privateSpendKey, Crypto::SecretKey privateViewKey,
    const std::string filename, const std::string password,
    const uint64_t scanHeight, const std::string daemonHost,
    const uint16_t daemonPort)
{
    /* Check the filename is valid */
    if (WalletError error = checkNewWalletFilename(filename); error != SUCCESS)
    {
        return {error, WalletBackend()};
    }

    /* Just defining here so it's more obvious what we're doing in the
       constructor */
    bool newWallet = false;
    bool isViewWallet = false;

    auto wallet = WalletBackend(
        filename, password, privateSpendKey, privateViewKey, isViewWallet,
        scanHeight, newWallet, daemonHost, daemonPort
    );

    if (WalletError error = wallet.init(); error != SUCCESS)
    {
        return {error, WalletBackend()};
    }

    /* Save to disk */
    WalletError error = wallet.save();

    return {error, std::move(wallet)};
}

/* Imports a view wallet from a private view key and an address.
   Returns the wallet class, or an error. */
/* TODO: Parse address into public spend key, pass to synchronizer */
std::tuple<WalletError, WalletBackend> WalletBackend::importViewWallet(
    const Crypto::SecretKey privateViewKey, const std::string address,
    const std::string filename, const std::string password,
    const uint64_t scanHeight, const std::string daemonHost,
    const uint16_t daemonPort)
{
    /* Check the filename is valid */
    if (WalletError error = checkNewWalletFilename(filename); error != SUCCESS)
    {
        return {error, WalletBackend()};
    }

    /* Just defining here so it's more obvious what we're doing in the
       constructor */
    bool newWallet = false;
    bool isViewWallet = true;

    auto wallet = WalletBackend(
        filename, password, CryptoNote::NULL_SECRET_KEY, privateViewKey,
        isViewWallet, scanHeight, newWallet, daemonHost, daemonPort
    );

    if (WalletError error = wallet.init(); error != SUCCESS)
    {
        return {error, WalletBackend()};
    }

    /* Save to disk */
    WalletError error = wallet.save();

    return {error, std::move(wallet)};
}

/* Creates a new wallet with the given filename and password */
std::tuple<WalletError, WalletBackend> WalletBackend::createWallet(
    const std::string filename, const std::string password,
    const std::string daemonHost, const uint16_t daemonPort)
{
    /* Check the filename is valid */
    if (WalletError error = checkNewWalletFilename(filename); error != SUCCESS)
    {
        return {error, WalletBackend()};
    }

    CryptoNote::KeyPair spendKey;
    Crypto::SecretKey privateViewKey;
    Crypto::PublicKey publicViewKey;

    /* Generate a spend key */
    Crypto::generate_keys(spendKey.publicKey, spendKey.secretKey);

    /* Derive the view key from the spend key */
    CryptoNote::AccountBase::generateViewFromSpend(
        spendKey.secretKey, privateViewKey, publicViewKey
    );

    /* Just defining here so it's more obvious what we're doing in the
       constructor */
    bool newWallet = true;
    bool isViewWallet = false;
    uint64_t scanHeight = 0;

    auto wallet = WalletBackend(
        filename, password, spendKey.secretKey, privateViewKey, isViewWallet,
        scanHeight, newWallet, daemonHost, daemonPort
    );

    if (WalletError error = wallet.init(); error != SUCCESS)
    {
        return {error, WalletBackend()};
    }

    /* Save to disk */
    WalletError error = wallet.save();

    return {error, std::move(wallet)};
}

/* Opens a wallet already on disk with the given filename + password */
std::tuple<WalletError, WalletBackend> WalletBackend::openWallet(
    const std::string filename, const std::string password,
    const std::string daemonHost, const uint16_t daemonPort)
{
    /* Open in binary mode, since we have encrypted data */
    std::ifstream file(filename, std::ios::binary);

    /* Check we successfully opened the file */
    if (!file)
    {
        return {FILENAME_NON_EXISTENT, WalletBackend()};
    }

    /* Read file into a buffer */
    std::vector<char> buffer((std::istreambuf_iterator<char>(file)),
                             (std::istreambuf_iterator<char>()));

    /* Check that the decrypted data has the 'isAWallet' identifier,
       and remove it it does. If it doesn't, return an error. */
    WalletError error = hasMagicIdentifier(
        buffer, Constants::IS_A_WALLET_IDENTIFIER.begin(),
        Constants::IS_A_WALLET_IDENTIFIER.end(),
        NOT_A_WALLET_FILE, NOT_A_WALLET_FILE
    );

    if (error)
    {
        return {error, WalletBackend()};
    }

    /* The salt we use for both PBKDF2, and AES decryption */
    CryptoPP::byte salt[16];

    /* Check the file is large enough for the salt */
    if (buffer.size() < sizeof(salt))
    {
        return {WALLET_FILE_CORRUPTED, WalletBackend()};
    }

    /* Copy the salt to the salt array */
    std::copy(buffer.begin(), buffer.begin() + sizeof(salt), salt);

    /* Remove the salt, don't need it anymore */
    buffer.erase(buffer.begin(), buffer.begin() + sizeof(salt));

    /* The key we use for AES decryption, generated with PBKDF2 */
    CryptoPP::byte key[32];

    /* Using SHA256 as the algorithm */
    CryptoPP::PKCS5_PBKDF2_HMAC<CryptoPP::SHA256> pbkdf2;

    /* Generate the AES Key using pbkdf2 */
    pbkdf2.DeriveKey(
        key, sizeof(key), 0, (CryptoPP::byte *)password.data(),
        password.size(), salt, sizeof(salt), Constants::PBKDF2_ITERATIONS
    );

    /* Intialize aesDecryption with the AES Key */
    CryptoPP::AES::Decryption aesDecryption(key, sizeof(key));

    /* Using CBC encryption, pass in the salt */
    CryptoPP::CBC_Mode_ExternalCipher::Decryption cbcDecryption(
        aesDecryption, salt
    );

    /* This will store the decrypted data */
    std::string decryptedData;

    /* Stream the decrypted data into the decryptedData string */
    CryptoPP::StreamTransformationFilter stfDecryptor(
        cbcDecryption, new CryptoPP::StringSink(decryptedData)
    );

    /* Write the data to the AES decryptor stream */
    stfDecryptor.Put(reinterpret_cast<const CryptoPP::byte *>(buffer.data()),
                     buffer.size());

    stfDecryptor.MessageEnd();

    /* Check that the decrypted data has the 'isCorrectPassword' identifier,
       and remove it it does. If it doesn't, return an error. */
    error = hasMagicIdentifier(
        decryptedData, Constants::IS_CORRECT_PASSWORD_IDENTIFIER.begin(),
        Constants::IS_CORRECT_PASSWORD_IDENTIFIER.end(), WALLET_FILE_CORRUPTED,
        WRONG_PASSWORD
    );

    if (error)
    {
        return {error, WalletBackend()};
    }

    WalletBackend wallet = json::parse(decryptedData);

    /* Since json::parse() uses the default constructor, the node, filename,
       and password won't be initialized. */
    error = wallet.initializeAfterLoad(
        filename, password, daemonHost, daemonPort
    );

    return {error, std::move(wallet)};
}

WalletError WalletBackend::initializeAfterLoad(std::string filename,
    std::string password, std::string daemonHost, uint16_t daemonPort)
{
    m_filename = filename;
    m_password = password;

    m_daemon = std::make_shared<CryptoNote::NodeRpcProxy>(
        daemonHost, daemonPort, m_logger->getLogger()
    );

    m_eventHandler = std::make_shared<EventHandler>();

    return init();
}

WalletError WalletBackend::init()
{
    if (m_daemon == nullptr)
    {
        throw std::runtime_error("Daemon has not been initialized!");
    }

    std::cout << "Initializing daemon, this may hang..." << std::endl;

    std::promise<std::error_code> errorPromise;
    std::future<std::error_code> error = errorPromise.get_future();

    auto callback = [&errorPromise](std::error_code e) 
    {
        errorPromise.set_value(e);
    };

    m_daemon->init(callback);

    std::future<WalletError> initDaemon = std::async(std::launch::async,
                                                    [&error]
    {
        if (error.get())
        {
            return FAILED_TO_INIT_DAEMON;
        }
        else
        {
            return SUCCESS;
        }
    });


    /* Wait for the daemon to init */
    /* TODO: This can hang - can't do it in a std::future since that hangs
       when going of out scope */
    WalletError result = initDaemon.get();

    /* Init the wallet synchronizer if it hasn't been loaded from the wallet
       file */
    if (m_walletSynchronizer == nullptr)
    {
        auto [startHeight, startTimestamp] = m_subWallets->getMinInitialSyncStart();

        m_walletSynchronizer = std::make_shared<WalletSynchronizer>(
            m_daemon, 
            startHeight,
            startTimestamp,
            m_privateViewKey,
            m_eventHandler
        );
    }
    /* If it has, just initialize the stuff we can't from file */
    else
    {
        m_walletSynchronizer->initializeAfterLoad(m_daemon, m_eventHandler);
    }

    m_walletSynchronizer->m_subWallets = m_subWallets;

    /* Launch the wallet sync process in a background thread */
    m_walletSynchronizer->start();

    std::cout << "Daemon initialization completed!" << std::endl;

    return result;
}

WalletError WalletBackend::save() const
{
    /* Add an identifier to the start of the string so we can verify the wallet
       has been correctly decrypted */
    std::string identiferAsString(
        Constants::IS_CORRECT_PASSWORD_IDENTIFIER.begin(),
        Constants::IS_CORRECT_PASSWORD_IDENTIFIER.end()
    );

    /* Serialize wallet to json */
    json walletJson = *this;

    /* Add magic identifier, and get json as a string */
    std::string walletData = identiferAsString + walletJson.dump();

    /* The key we use for AES encryption, generated with PBKDF2 */
    CryptoPP::byte key[32];

    /* The salt we use for both PBKDF2, and AES Encryption */
    CryptoPP::byte salt[16];

    /* Generate 16 random bytes for the salt */
    Crypto::generate_random_bytes(16, salt);

    /* Using SHA256 as the algorithm */
    CryptoPP::PKCS5_PBKDF2_HMAC<CryptoPP::SHA256> pbkdf2;

    /* Generate the AES Key using pbkdf2 */
    pbkdf2.DeriveKey(
        key, sizeof(key), 0, (CryptoPP::byte *)m_password.data(),
        m_password.size(), salt, sizeof(salt), Constants::PBKDF2_ITERATIONS
    );

    CryptoPP::AES::Encryption aesEncryption(key, sizeof(key));

    /* Using the CBC mode of AES encryption */
    CryptoPP::CBC_Mode_ExternalCipher::Encryption cbcEncryption(
        aesEncryption, salt
    );

    /* This will store the encrypted data */
    std::string encryptedData;

    CryptoPP::StreamTransformationFilter stfEncryptor(
        cbcEncryption, new CryptoPP::StringSink(encryptedData)
    );

    /* Write the data to the AES stream */
    stfEncryptor.Put(
        reinterpret_cast<const CryptoPP::byte *>(walletData.c_str()),
        walletData.length()
    );

    stfEncryptor.MessageEnd();

    std::ofstream file(m_filename);

    if (!file)
    {
        return INVALID_WALLET_FILENAME;
    }

    /* Get the isAWalletIdentifier array as a string */
    std::string walletPrefix = std::string(
        Constants::IS_A_WALLET_IDENTIFIER.begin(),
        Constants::IS_A_WALLET_IDENTIFIER.end()
    );

    /* Get the salt array as a string */
    std::string saltString = std::string(salt, salt + sizeof(salt));

    /* Write the isAWalletIdentifier to the file, so when we open it we can
       verify that it is a wallet file */
    file << walletPrefix;

    /* Write the salt to the file, so we can use it to unencrypt the file
       later. Note that the salt is unencrypted. */
    file << saltString;

    /* Write the encrypted wallet data to the file */
    file << encryptedData;

    return SUCCESS;
}

std::tuple<WalletError, uint64_t> WalletBackend::getBalance(
    const std::string address) const
{
    /* Verify the address is good, and one of our subwallets */
    if (WalletError error = validateOurAddresses({address}, *m_subWallets); error != SUCCESS)
    {
        return {error, 0};
    }

    uint64_t balance = m_subWallets->getBalance(
        addressesToSpendKeys({address}),
        false
    );

    return {SUCCESS, balance};
}

/* Gets the combined balance for all wallets in the container */
uint64_t WalletBackend::getTotalBalance() const
{
    /* Get combined balance from every container */
    return m_subWallets->getBalance({}, true);
}