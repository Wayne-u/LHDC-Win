#include "signature.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <mscat.h>
#include <stdexcept>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace lhdc {
namespace {
[[noreturn]] void error(const char* operation) {
    throw std::runtime_error(std::string(operation)+" failed, Win32="+std::to_string(GetLastError()));
}
struct Certificate {
    PCCERT_CONTEXT value=nullptr;
    ~Certificate(){if(value) CertFreeCertificateContext(value);}
};
struct Message {
    HCERTSTORE store=nullptr;
    HCRYPTMSG value=nullptr;
    ~Message(){if(value) CryptMsgClose(value);if(store) CertCloseStore(store,0);}
};
struct File {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~File(){if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
};
struct Catalog {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Catalog(){if(value!=INVALID_HANDLE_VALUE) CryptCATClose(value);}
};
class Hasher {
public:
    Hasher() {
        library_=LoadLibraryExW(L"wintrust.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_) error("Wintrust library");
        const auto acquire=reinterpret_cast<decltype(&CryptCATAdminAcquireContext2)>(GetProcAddress(library_,"CryptCATAdminAcquireContext2"));
        calculate_=reinterpret_cast<decltype(&CryptCATAdminCalcHashFromFileHandle2)>(GetProcAddress(library_,"CryptCATAdminCalcHashFromFileHandle2"));
        if (!acquire || !calculate_) {
            FreeLibrary(library_);library_=nullptr;
            throw std::runtime_error("Windows SHA256 catalog hashing API is unavailable");
        }
        if (!acquire(&context_,nullptr,L"SHA256",nullptr,0)) {
            const auto code=GetLastError();FreeLibrary(library_);library_=nullptr;
            SetLastError(code);error("Catalog hash context");
        }
    }
    ~Hasher(){if(context_) CryptCATAdminReleaseContext(context_,0);if(library_) FreeLibrary(library_);}
    std::vector<BYTE> hash(const std::filesystem::path& path) {
        File file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr)};
        if (file.value==INVALID_HANDLE_VALUE) error("Hash file open");
        DWORD size=0;
        if (!calculate_(context_,file.value,&size,nullptr,0)) error("File digest size");
        std::vector<BYTE> result(size);
        if (!calculate_(context_,file.value,&size,result.data(),0)) error("File digest");
        result.resize(size);
        return result;
    }
private:
    HMODULE library_=nullptr;
    HCATADMIN context_=nullptr;
    decltype(&CryptCATAdminCalcHashFromFileHandle2) calculate_=nullptr;
};
std::vector<BYTE> message_param(HCRYPTMSG message,DWORD parameter) {
    DWORD size=0;
    if (!CryptMsgGetParam(message,parameter,0,nullptr,&size)) error("Signature parameter size");
    std::vector<BYTE> result(size);
    if (!CryptMsgGetParam(message,parameter,0,result.data(),&size)) error("Signature parameter");
    result.resize(size);return result;
}
void verify_message(const std::filesystem::path& path,PCCERT_CONTEXT certificate,Hasher& hasher,bool pe) {
    Message message;
    const auto formats=CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED | CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE,path.c_str(),formats,CERT_QUERY_FORMAT_FLAG_BINARY,0,
        nullptr,nullptr,nullptr,&message.store,&message.value,nullptr)) error("Signed file decode");
    DWORD count=0,size=sizeof(count);
    if (!CryptMsgGetParam(message.value,CMSG_SIGNER_COUNT_PARAM,0,&count,&size)) error("Signer count");
    if (count!=1) throw std::runtime_error("Expected one test-package signer");
    CMSG_CTRL_VERIFY_SIGNATURE_EX_PARA verify{};
    verify.cbSize=sizeof(verify);verify.dwSignerIndex=0;verify.dwSignerType=CMSG_VERIFY_SIGNER_CERT;
    verify.pvSigner=const_cast<PCERT_CONTEXT>(certificate);
    // Verify the signed message with the explicit test public key. No trust
    // stores are changed, and no claim about kernel loading policy is made.
    if (!CryptMsgControl(message.value,0,CMSG_CTRL_VERIFY_SIGNATURE_EX,&verify)) error("Signed message signature");
    if (!pe) return;
    const auto content=message_param(message.value,CMSG_CONTENT_PARAM);
    DWORD decoded_size=0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,SPC_INDIRECT_DATA_CONTENT_STRUCT,
        content.data(),static_cast<DWORD>(content.size()),0,nullptr,nullptr,&decoded_size)) error("PE indirect digest size");
    std::vector<BYTE> decoded(decoded_size);
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,SPC_INDIRECT_DATA_CONTENT_STRUCT,
        content.data(),static_cast<DWORD>(content.size()),0,nullptr,decoded.data(),&decoded_size)) error("PE indirect digest");
    const auto data=reinterpret_cast<const SPC_INDIRECT_DATA_CONTENT*>(decoded.data());
    if (std::strcmp(data->DigestAlgorithm.pszObjId,szOID_NIST_sha256)!=0) throw std::runtime_error("Expected a SHA256 PE signature");
    const auto actual=hasher.hash(path);
    if (actual.size()!=data->Digest.cbData || !std::equal(actual.begin(),actual.end(),data->Digest.pbData))
        throw std::runtime_error("SYS Authenticode file digest does not match its signed message");
}
void verify_catalog_member(HANDLE catalog,const std::filesystem::path& file,Hasher& hasher) {
    const auto expected=hasher.hash(file);
    CRYPTCATMEMBER* member=nullptr;
    while ((member=CryptCATEnumerateMember(catalog,member))!=nullptr) {
        const auto data=member->pIndirectData;
        if (data && std::strcmp(data->DigestAlgorithm.pszObjId,szOID_NIST_sha256)==0 && data->Digest.cbData==expected.size() &&
            std::equal(expected.begin(),expected.end(),data->Digest.pbData)) return;
    }
    throw std::runtime_error("A package file digest is absent from the signed catalog");
}
}
void verify_package_signature(const std::filesystem::path& directory,const std::filesystem::path& certificate_file,const std::wstring& name) {
    Certificate certificate;
    const void* certificate_context=nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE,certificate_file.c_str(),CERT_QUERY_CONTENT_FLAG_CERT,
        CERT_QUERY_FORMAT_FLAG_BINARY,0,nullptr,nullptr,nullptr,nullptr,nullptr,&certificate_context)) error("Public test certificate");
    certificate.value=static_cast<PCCERT_CONTEXT>(certificate_context);
    if (CertVerifyTimeValidity(nullptr,certificate.value->pCertInfo)!=0) throw std::runtime_error("Test certificate is outside its validity period");
    Hasher hasher;
    const auto sys=directory/(name+L".sys"),cat=directory/(name+L".cat");
    verify_message(sys,certificate.value,hasher,true);
    verify_message(cat,certificate.value,hasher,false);
    auto cat_path=cat.wstring();
    Catalog catalog{CryptCATOpen(cat_path.data(),CRYPTCAT_OPEN_EXISTING,0,0,0)};
    if (catalog.value==INVALID_HANDLE_VALUE) error("Catalog open");
    verify_catalog_member(catalog.value,sys,hasher);
    verify_catalog_member(catalog.value,directory/(name+L".inf"),hasher);
    BYTE thumbprint[20]{};
    DWORD size=sizeof(thumbprint);
    if (!CertGetCertificateContextProperty(certificate.value,CERT_SHA1_HASH_PROP_ID,thumbprint,&size)) error("Certificate thumbprint");
    std::ostringstream hex;
    hex<<std::hex<<std::setfill('0');
    for (BYTE byte:thumbprint) hex<<std::setw(2)<<static_cast<unsigned>(byte);
    std::cout << "{\"event\":\"package_signature_verified\",\"certificate_thumbprint\":\"" << hex.str()
        << "\",\"sys_signature\":true,\"sys_digest\":true,\"catalog_signature\":true,\"catalog_members\":true,\"kernel_trust_verified\":false}\n";
}
}
