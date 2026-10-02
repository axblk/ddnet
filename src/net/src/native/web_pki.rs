//! Web PKI, `#webpki`: a server's certificate is checked against the
//! roots browsers trust, for the name it was connected by, the way a
//! browser checks it. The roots are built in, so the check is the same on
//! every system.

use rustls_pki_types::CertificateDer;
use rustls_pki_types::ServerName;
use rustls_pki_types::UnixTime;
use webpki::ring as algs;
use webpki::EndEntityCert;
use webpki::KeyUsage;

/// The signature algorithms the roots and the certificates under them use.
static ALGORITHMS: &[&dyn rustls_pki_types::SignatureVerificationAlgorithm] = &[
    algs::ECDSA_P256_SHA256,
    algs::ECDSA_P256_SHA384,
    algs::ECDSA_P384_SHA256,
    algs::ECDSA_P384_SHA384,
    algs::ED25519,
    algs::RSA_PKCS1_2048_8192_SHA256,
    algs::RSA_PKCS1_2048_8192_SHA384,
    algs::RSA_PKCS1_2048_8192_SHA512,
    algs::RSA_PKCS1_3072_8192_SHA384,
    algs::RSA_PSS_2048_8192_SHA256_LEGACY_KEY,
    algs::RSA_PSS_2048_8192_SHA384_LEGACY_KEY,
    algs::RSA_PSS_2048_8192_SHA512_LEGACY_KEY,
];

/// Checks the certificate a server showed, `chain[0]`, with the
/// intermediates it sent after it, for `name`: a host name or an IP
/// address.
pub(crate) fn verify(chain: &[Vec<u8>], name: &str) -> Result<(), String> {
    let (leaf, intermediates) = chain.split_first().ok_or("no certificate")?;
    let leaf = CertificateDer::from(&leaf[..]);
    let intermediates: Vec<_> = intermediates.iter().map(|der| CertificateDer::from(&der[..])).collect();
    let name = ServerName::try_from(name).map_err(|error| error.to_string())?;
    let certificate = EndEntityCert::try_from(&leaf).map_err(|error| error.to_string())?;
    certificate
        .verify_for_usage(
            ALGORITHMS,
            webpki_roots::TLS_SERVER_ROOTS,
            &intermediates,
            UnixTime::now(),
            KeyUsage::server_auth(),
            None,
            None,
        )
        .map_err(|error| error.to_string())?;
    certificate.verify_is_valid_for_subject_name(&name).map_err(|error| error.to_string())
}

/// The DER of a chain boring hands out.
pub(crate) fn der_chain<'a>(certificates: impl Iterator<Item = &'a boring::x509::X509Ref>) -> Vec<Vec<u8>> {
    certificates.filter_map(|certificate| certificate.to_der().ok()).collect()
}

#[cfg(test)]
mod test {
    use super::verify;
    use crate::key::unix_now;
    use crate::key::BrowserCertificate;

    #[test]
    fn a_certificate_without_a_public_root_is_refused() {
        let certificate = BrowserCertificate::generate(unix_now());
        let chain: Vec<_> = certificate.chain().iter().map(|certificate| certificate.to_der().unwrap()).collect();
        let error = verify(&chain, "localhost").unwrap_err();
        assert!(error.contains("UnknownIssuer"), "{}", error);
        assert!(verify(&[], "localhost").is_err());
        assert!(verify(&chain, "not a name").is_err());
    }
}
