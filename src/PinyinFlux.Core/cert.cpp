#include "cert.h"

#include <ncrypt.h>

#include "log.h"

namespace pf {

namespace {

constexpr wchar_t kKeyName[] = L"PinyinFlux-TLS";
constexpr wchar_t kSubject[] = L"CN=cn.bing.com, O=PinyinFlux (local)";
constexpr const wchar_t* kDnsNames[] = {L"cn.bing.com", L"www.bing.com"};
constexpr ULONGLONG kRenewBeforeMs = 30ull * 24 * 3600 * 1000;

bool IsOurs(PCCERT_CONTEXT cert) {
  wchar_t name[256];
  DWORD size = sizeof(name);
  return CertGetCertificateContextProperty(cert, CERT_FRIENDLY_NAME_PROP_ID, name, &size) &&
         wcscmp(name, kCertFriendlyName) == 0;
}

bool ExpiresSoon(PCCERT_CONTEXT cert) {
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER n{{now.dwLowDateTime, now.dwHighDateTime}};
  ULARGE_INTEGER end{{cert->pCertInfo->NotAfter.dwLowDateTime, cert->pCertInfo->NotAfter.dwHighDateTime}};
  return end.QuadPart < n.QuadPart + kRenewBeforeMs * 10000;
}

PCCERT_CONTEXT FindIn(HCERTSTORE store, PCCERT_CONTEXT match) {
  PCCERT_CONTEXT cert = nullptr;
  while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
    if (match ? CertCompareCertificate(X509_ASN_ENCODING, cert->pCertInfo, match->pCertInfo) : IsOurs(cert)) {
      return CertDuplicateCertificateContext(cert);  // enumeration context is freed below
    }
  }
  return nullptr;
}

HCERTSTORE OpenUserStore(const wchar_t* name) {
  return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, name);
}

void DeleteKey() {
  NCRYPT_PROV_HANDLE prov = 0;
  NCRYPT_KEY_HANDLE key = 0;
  if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return;
  if (NCryptOpenKey(prov, &key, kKeyName, 0, 0) == ERROR_SUCCESS) NCryptDeleteKey(key, 0);
  NCryptFreeObject(prov);
}

NCRYPT_KEY_HANDLE CreateKey() {
  NCRYPT_PROV_HANDLE prov = 0;
  NCRYPT_KEY_HANDLE key = 0;
  if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return 0;
  SECURITY_STATUS st = NCryptCreatePersistedKey(prov, &key, NCRYPT_RSA_ALGORITHM, kKeyName, 0,
                                                NCRYPT_OVERWRITE_KEY_FLAG);
  if (st == ERROR_SUCCESS) {
    DWORD length = 2048;
    DWORD exportPolicy = 0;  // non-exportable
    DWORD usage = NCRYPT_ALLOW_ALL_USAGES;
    NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&length), sizeof(length), 0);
    NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&exportPolicy),
                      sizeof(exportPolicy), NCRYPT_PERSIST_FLAG);
    NCryptSetProperty(key, NCRYPT_KEY_USAGE_PROPERTY, reinterpret_cast<PBYTE>(&usage), sizeof(usage),
                      NCRYPT_PERSIST_FLAG);
    st = NCryptFinalizeKey(key, 0);
  }
  NCryptFreeObject(prov);
  if (st != ERROR_SUCCESS) {
    Log("cert: key creation failed 0x%08lx", st);
    if (key) NCryptFreeObject(key);
    return 0;
  }
  return key;
}

// DER-encodes a certificate extension value; returns false on failure.
bool Encode(LPCSTR structType, const void* value, std::vector<BYTE>& out) {
  DWORD size = 0;
  if (!CryptEncodeObject(X509_ASN_ENCODING, structType, value, nullptr, &size)) return false;
  out.resize(size);
  return CryptEncodeObject(X509_ASN_ENCODING, structType, value, out.data(), &size) != FALSE;
}

PCCERT_CONTEXT CreateCertificate() {
  DeleteKey();
  NCRYPT_KEY_HANDLE key = CreateKey();
  if (!key) return nullptr;

  std::vector<BYTE> subject;
  DWORD size = 0;
  CertStrToNameW(X509_ASN_ENCODING, kSubject, CERT_X500_NAME_STR, nullptr, nullptr, &size, nullptr);
  subject.resize(size);
  CertStrToNameW(X509_ASN_ENCODING, kSubject, CERT_X500_NAME_STR, nullptr, subject.data(), &size, nullptr);
  CERT_NAME_BLOB subjectBlob{size, subject.data()};

  CERT_ALT_NAME_ENTRY names[ARRAYSIZE(kDnsNames)];
  for (size_t i = 0; i < ARRAYSIZE(kDnsNames); ++i) {
    names[i].dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
    names[i].pwszDNSName = const_cast<LPWSTR>(kDnsNames[i]);
  }
  CERT_ALT_NAME_INFO altNames{ARRAYSIZE(names), names};

  LPSTR serverAuth = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
  CERT_ENHKEY_USAGE eku{1, &serverAuth};

  CERT_BASIC_CONSTRAINTS2_INFO basic{FALSE, FALSE, 0};

  BYTE keyUsageBits = CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE;
  CRYPT_BIT_BLOB keyUsage{1, &keyUsageBits, 0};

  std::vector<BYTE> altEnc, ekuEnc, basicEnc, usageEnc;
  if (!Encode(X509_ALTERNATE_NAME, &altNames, altEnc) || !Encode(X509_ENHANCED_KEY_USAGE, &eku, ekuEnc) ||
      !Encode(X509_BASIC_CONSTRAINTS2, &basic, basicEnc) || !Encode(X509_KEY_USAGE, &keyUsage, usageEnc)) {
    NCryptFreeObject(key);
    return nullptr;
  }
  CERT_EXTENSION exts[] = {
      {const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2), FALSE, {static_cast<DWORD>(altEnc.size()), altEnc.data()}},
      {const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE), FALSE, {static_cast<DWORD>(ekuEnc.size()), ekuEnc.data()}},
      {const_cast<LPSTR>(szOID_BASIC_CONSTRAINTS2), TRUE, {static_cast<DWORD>(basicEnc.size()), basicEnc.data()}},
      {const_cast<LPSTR>(szOID_KEY_USAGE), TRUE, {static_cast<DWORD>(usageEnc.size()), usageEnc.data()}},
  };
  CERT_EXTENSIONS extensions{ARRAYSIZE(exts), exts};

  CRYPT_KEY_PROV_INFO provInfo{};
  provInfo.pwszContainerName = const_cast<LPWSTR>(kKeyName);
  provInfo.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
  provInfo.dwProvType = 0;  // 0 = CNG key storage provider
  provInfo.dwKeySpec = AT_KEYEXCHANGE;

  CRYPT_ALGORITHM_IDENTIFIER sigAlg{const_cast<LPSTR>(szOID_RSA_SHA256RSA), {}};

  SYSTEMTIME start, end;
  GetSystemTime(&start);
  FILETIME ft;
  SystemTimeToFileTime(&start, &ft);
  ULARGE_INTEGER t{{ft.dwLowDateTime, ft.dwHighDateTime}};
  t.QuadPart -= 24ull * 3600 * 10000000;  // tolerate clock skew
  ft = {t.LowPart, t.HighPart};
  FileTimeToSystemTime(&ft, &start);
  end = start;
  end.wYear += 2;
  if (end.wMonth == 2 && end.wDay == 29) end.wDay = 28;

  PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(key, &subjectBlob, 0, &provInfo, &sigAlg, &start, &end,
                                                      &extensions);
  NCryptFreeObject(key);
  if (!cert) {
    Log("cert: CertCreateSelfSignCertificate failed %lu", GetLastError());
    return nullptr;
  }

  CRYPT_DATA_BLOB friendly{static_cast<DWORD>((wcslen(kCertFriendlyName) + 1) * sizeof(wchar_t)),
                           reinterpret_cast<BYTE*>(const_cast<wchar_t*>(kCertFriendlyName))};
  CertSetCertificateContextProperty(cert, CERT_FRIENDLY_NAME_PROP_ID, 0, &friendly);

  PCCERT_CONTEXT stored = nullptr;
  if (HCERTSTORE my = OpenUserStore(L"MY")) {
    CertAddCertificateContextToStore(my, cert, CERT_STORE_ADD_REPLACE_EXISTING, &stored);
    CertCloseStore(my, 0);
  }
  CertFreeCertificateContext(cert);
  if (stored) Log("cert: created interception certificate");
  return stored;
}

}  // namespace

PCCERT_CONTEXT FindCertificate() {
  HCERTSTORE my = OpenUserStore(L"MY");
  if (!my) return nullptr;
  PCCERT_CONTEXT cert = FindIn(my, nullptr);
  CertCloseStore(my, 0);
  if (cert && ExpiresSoon(cert)) {
    CertFreeCertificateContext(cert);
    return nullptr;
  }
  return cert;
}

PCCERT_CONTEXT EnsureCertificate() {
  if (PCCERT_CONTEXT cert = FindCertificate()) return cert;
  RemoveCertificate();  // drop any expiring leftovers before minting a new one
  return CreateCertificate();
}

bool IsCertificateTrusted(PCCERT_CONTEXT cert) {
  HCERTSTORE root = OpenUserStore(L"ROOT");
  if (!root) return false;
  PCCERT_CONTEXT found = FindIn(root, cert);
  CertCloseStore(root, 0);
  if (found) CertFreeCertificateContext(found);
  return found != nullptr;
}

bool TrustCertificate(PCCERT_CONTEXT cert) {
  if (IsCertificateTrusted(cert)) return true;
  HCERTSTORE root = OpenUserStore(L"ROOT");
  if (!root) return false;
  // Only the public certificate goes into Root; the key stays bound to My.
  BOOL ok = CertAddEncodedCertificateToStore(root, X509_ASN_ENCODING, cert->pbCertEncoded, cert->cbCertEncoded,
                                             CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
  DWORD err = GetLastError();
  CertCloseStore(root, 0);
  if (!ok) Log("cert: trusting certificate failed or was declined (%lu)", err);
  return ok != FALSE;
}

void RemoveCertificate() {
  for (const wchar_t* name : {L"ROOT", L"MY"}) {
    HCERTSTORE store = OpenUserStore(name);
    if (!store) continue;
    std::vector<PCCERT_CONTEXT> doomed;
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
      // Root copies carry no friendly name, so match them by subject instead.
      wchar_t subject[256];
      CertNameToStrW(X509_ASN_ENCODING, &cert->pCertInfo->Subject, CERT_X500_NAME_STR, subject, ARRAYSIZE(subject));
      if (IsOurs(cert) || wcscmp(subject, kSubject) == 0) doomed.push_back(CertDuplicateCertificateContext(cert));
    }
    for (PCCERT_CONTEXT c : doomed) CertDeleteCertificateFromStore(c);  // also frees the context
    CertCloseStore(store, 0);
  }
  DeleteKey();
}

}  // namespace pf
