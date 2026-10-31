#pragma once

#include "common.h"

#include <wincrypt.h>

namespace pf {

// The interception certificate: self-signed, valid only for the Bing hosts the
// IME talks to, with a non-exportable CNG key stored in the user profile.
inline constexpr wchar_t kCertFriendlyName[] = L"PinyinFlux local interception (cn.bing.com)";

// Finds the existing certificate in CurrentUser\My (nullptr if none or expiring).
PCCERT_CONTEXT FindCertificate();

// Creates the key + certificate in CurrentUser\My if needed. Caller frees.
PCCERT_CONTEXT EnsureCertificate();

// Adds the certificate to CurrentUser\Root. Windows asks the user to confirm.
bool TrustCertificate(PCCERT_CONTEXT cert);
bool IsCertificateTrusted(PCCERT_CONTEXT cert);

// Removes the certificate from My and Root and deletes its private key.
void RemoveCertificate();

}  // namespace pf
