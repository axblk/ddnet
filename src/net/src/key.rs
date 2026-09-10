#[cfg(not(target_os = "emscripten"))]
use crate::secure_random;
use crate::Error;
#[cfg(not(target_os = "emscripten"))]
use foreign_types_shared::ForeignType as _;
#[cfg(not(target_os = "emscripten"))]
use foreign_types_shared::ForeignTypeRef as _;
use std::fmt;
#[cfg(not(target_os = "emscripten"))]
use std::fs;
#[cfg(not(target_os = "emscripten"))]
use std::ptr;
use std::str;
use std::str::FromStr;
#[cfg(not(target_os = "emscripten"))]
use std::time::SystemTime;
#[cfg(not(target_os = "emscripten"))]
use std::time::UNIX_EPOCH;

/// A browser takes a certificate by its hash only if it is valid for at
/// most two weeks; this one is valid for thirteen days and an hour, from
/// an hour ago.
#[cfg(not(target_os = "emscripten"))]
const BROWSER_CERTIFICATE_LIFETIME: i64 = 13 * 24 * 60 * 60;
#[cfg(not(target_os = "emscripten"))]
const BROWSER_CERTIFICATE_BACKDATE: i64 = 60 * 60;
/// A server swaps its browser certificate for the next one after a week;
/// the next one's hash is known a week in advance, so a server list a
/// client fetched before the swap still works.
#[cfg(not(target_os = "emscripten"))]
pub const BROWSER_CERTIFICATE_ROTATION: i64 = 7 * 24 * 60 * 60;
/// The certificate the identity itself sits in is only ever checked by
/// our own verify callback, which reads the key and ignores the dates;
/// it is still remade long before it runs out, for any other TLS stack
/// that looks at them.
#[cfg(not(target_os = "emscripten"))]
pub const IDENTITY_CERTIFICATE_LIFETIME: i64 = 7 * 24 * 60 * 60;
#[cfg(not(target_os = "emscripten"))]
pub const IDENTITY_CERTIFICATE_RENEWAL: i64 = 3 * 24 * 60 * 60;
/// What an identity signs to vouch for a certificate it does not sit in.
const IDENTITY_PROOF_CONTEXT: &[u8] = b"ddnet server identity v1\0";
pub const IDENTITY_PROOF_SIZE: usize = 32 + 64;

#[cfg(not(target_os = "emscripten"))]
pub fn unix_now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs() as i64)
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub struct Identity([u8; 32]);

impl fmt::Display for Identity {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        fmt::Debug::fmt(self, f)
    }
}

impl fmt::Debug for Identity {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        for b in self.0 {
            write!(f, "{:02x}", b)?;
        }
        Ok(())
    }
}

impl FromStr for Identity {
    type Err = Error;
    fn from_str(v: &str) -> Result<Identity, Error> {
        Ok(Identity::from_bytes(hex_to_32_bytes(v)?))
    }
}

#[cfg(not(target_os = "emscripten"))]
pub struct PrivateIdentity {
    lib: boring::pkey::PKey<boring::pkey::Private>,
}

#[cfg(not(target_os = "emscripten"))]
impl FromStr for PrivateIdentity {
    type Err = Error;
    fn from_str(v: &str) -> Result<PrivateIdentity, Error> {
        Ok(PrivateIdentity::from_bytes(hex_to_32_bytes(v)?))
    }
}

impl Identity {
    pub fn from_bytes(bytes: [u8; 32]) -> Identity {
        Identity(bytes)
    }
#[cfg(not(target_os = "emscripten"))]
    pub fn try_from_lib<T: boring::pkey::HasPublic>(
        key: &boring::pkey::PKeyRef<T>,
    ) -> Option<Identity> {
        if key.id() != boring::pkey::Id::ED25519 {
            return None;
        }
        unsafe {
            let mut buf = [0; 32];
            let mut len = buf.len();
            // TODO: expose this from the `boring` crate
            if boring_sys::EVP_PKEY_get_raw_public_key(
                key.as_ptr(),
                buf.as_mut_ptr(),
                &mut len,
            ) != 1
            {
                return None;
            }
            if len != buf.len() {
                return None;
            }
            Some(Identity::from_bytes(buf))
        }
    }
#[cfg(not(target_os = "emscripten"))]
    pub fn to_lib(&self) -> boring::pkey::PKey<boring::pkey::Public> {
        unsafe {
            // TODO: expose this from the `boring` crate
            let result = boring_sys::EVP_PKEY_new_raw_public_key(
                boring_sys::EVP_PKEY_ED25519,
                ptr::null_mut(),
                self.0.as_ptr(),
                self.0.len(),
            );
            assert!(!result.is_null());
            boring::pkey::PKey::from_ptr(result)
        }
    }
    pub fn as_bytes(&self) -> &[u8; 32] {
        &self.0
    }
    /// Checks an identity proof: that this identity vouched for the
    /// certificate a client saw, in answer to the client's nonce.
    pub fn verify_proof(&self, proof: &[u8], certificate_sha256: &[u8; 32], nonce: &[u8; 32]) -> Option<Identity> {
        if proof.len() != IDENTITY_PROOF_SIZE {
            return None;
        }
        let identity = Identity::from_bytes(proof[..32].try_into().unwrap());
        if identity != *self {
            return None;
        }
        let message = identity_proof_message(certificate_sha256, nonce);
        identity.verify(&message, &proof[32..]).then_some(identity)
    }
    #[cfg(not(target_os = "emscripten"))]
    fn verify(&self, message: &[u8], signature: &[u8]) -> bool {
        let key = self.to_lib();
        let Ok(mut verifier) = boring::sign::Verifier::new_without_digest(&key) else {
            return false;
        };
        verifier.verify_oneshot(signature, message).unwrap_or(false)
    }
    /// The browser build has no boringssl; the signature check is the
    /// one thing it needs of it.
    #[cfg(target_os = "emscripten")]
    fn verify(&self, message: &[u8], signature: &[u8]) -> bool {
        let Ok(key) = ed25519_dalek::VerifyingKey::from_bytes(&self.0) else {
            return false;
        };
        let Ok(signature) = ed25519_dalek::Signature::from_slice(signature) else {
            return false;
        };
        key.verify_strict(message, &signature).is_ok()
    }
}

fn identity_proof_message(certificate_sha256: &[u8; 32], nonce: &[u8; 32]) -> Vec<u8> {
    let mut message = Vec::with_capacity(IDENTITY_PROOF_CONTEXT.len() + 64);
    message.extend_from_slice(IDENTITY_PROOF_CONTEXT);
    message.extend_from_slice(certificate_sha256);
    message.extend_from_slice(nonce);
    message
}

/// A certificate for browsers, which take no Ed25519: ECDSA P-256, self-
/// signed and short-lived, or whatever the operator got from a CA.
#[cfg(not(target_os = "emscripten"))]
pub struct BrowserCertificate {
    key: boring::pkey::PKey<boring::pkey::Private>,
    chain: Vec<boring::x509::X509>,
    sha256: [u8; 32],
}

/// The `time_t` `boring` takes is 32 bit on some targets, Android among
/// them. Our certificates live days, not decades, so a time that does not
/// fit there is a bug rather than a case to handle.
#[cfg(not(target_os = "emscripten"))]
fn asn1_time(unix: i64) -> boring::asn1::Asn1Time {
    let unix = unix.try_into().expect("certificate time outside the platform's range");
    boring::asn1::Asn1Time::from_unix(unix).unwrap()
}

#[cfg(not(target_os = "emscripten"))]
impl BrowserCertificate {
    /// A fresh self-signed certificate, valid from `not_before`.
    pub fn generate(not_before: i64) -> BrowserCertificate {
        let group = boring::ec::EcGroup::from_curve_name(boring::nid::Nid::X9_62_PRIME256V1).unwrap();
        let key = boring::pkey::PKey::from_ec_key(boring::ec::EcKey::generate(&group).unwrap()).unwrap();
        let name = {
            let mut builder = boring::x509::X509Name::builder().unwrap();
            builder
                .append_entry_by_nid(boring::nid::Nid::COMMONNAME, "ddnet")
                .unwrap();
            builder.build()
        };
        let mut builder = boring::x509::X509::builder().unwrap();
        builder.set_version(2).unwrap();
        builder
            .set_serial_number(&random_bignum(160).to_asn1_integer().unwrap())
            .unwrap();
        builder.set_issuer_name(&name).unwrap();
        builder.set_subject_name(&name).unwrap();
        builder
            .set_not_before(&asn1_time(not_before - BROWSER_CERTIFICATE_BACKDATE))
            .unwrap();
        builder
            .set_not_after(&asn1_time(not_before + BROWSER_CERTIFICATE_LIFETIME))
            .unwrap();
        builder.set_pubkey(&key).unwrap();
        builder.sign(&key, boring::hash::MessageDigest::sha256()).unwrap();
        let cert = builder.build();
        BrowserCertificate::new(key, vec![cert])
    }
    /// A certificate chain and its key from PEM files, as a CA hands them
    /// out.
    pub fn from_files(cert_path: &str, key_path: &str) -> Result<BrowserCertificate, Error> {
        let cert_pem = fs::read(cert_path)
            .map_err(|e| Error::from_string(format!("{}: {}", cert_path, e)))?;
        let key_pem = fs::read(key_path)
            .map_err(|e| Error::from_string(format!("{}: {}", key_path, e)))?;
        let chain = boring::x509::X509::stack_from_pem(&cert_pem)
            .map_err(|e| Error::from_string(format!("{}: {}", cert_path, e)))?;
        if chain.is_empty() {
            bail!("{}: no certificate in the file", cert_path);
        }
        let key = boring::pkey::PKey::private_key_from_pem(&key_pem)
            .map_err(|e| Error::from_string(format!("{}: {}", key_path, e)))?;
        if !chain[0].public_key().map_or(false, |public| public.public_eq(&key)) {
            bail!("{} does not belong to the certificate in {}", key_path, cert_path);
        }
        Ok(BrowserCertificate::new(key, chain))
    }
    fn new(key: boring::pkey::PKey<boring::pkey::Private>, chain: Vec<boring::x509::X509>) -> BrowserCertificate {
        let digest = chain[0].digest(boring::hash::MessageDigest::sha256()).unwrap();
        BrowserCertificate {
            key,
            chain,
            sha256: digest.as_ref().try_into().unwrap(),
        }
    }
    pub fn key(&self) -> &boring::pkey::PKeyRef<boring::pkey::Private> {
        &self.key
    }
    /// The certificate first, then what leads up to the root.
    pub fn chain(&self) -> &[boring::x509::X509] {
        &self.chain
    }
    /// The hash a browser is given to accept the certificate by.
    pub fn sha256(&self) -> &[u8; 32] {
        &self.sha256
    }
}

fn hex_to_32_bytes(v: &str) -> Result<[u8; 32], Error> {
    if v.len() != 64 || !v.is_ascii() {
        bail!("invalid length {}, must be 64 hex digits", v.chars().count());
    }
    let mut result = [0; 32];
    for (i, pair) in v.as_bytes().chunks(2).enumerate() {
        // `from_str_radix` would also take a sign.
        if !pair.iter().all(u8::is_ascii_hexdigit) {
            bail!("non-hex character {:?} at index {}", str::from_utf8(pair).unwrap(), 2 * i);
        }
        result[i] = u8::from_str_radix(str::from_utf8(pair).unwrap(), 16).unwrap();
    }
    Ok(result)
}

#[cfg(not(target_os = "emscripten"))]
fn random_bignum(bits: i32) -> boring::bn::BigNum {
    let mut result = boring::bn::BigNum::new().unwrap();
    result
        .rand(bits, boring::bn::MsbOption::MAYBE_ZERO, false)
        .unwrap();
    result
}

#[cfg(not(target_os = "emscripten"))]
impl PrivateIdentity {
    pub fn random() -> PrivateIdentity {
        PrivateIdentity::from_bytes(secure_random())
    }
    pub fn public(&self) -> Identity {
        Identity::try_from_lib(&self.lib).unwrap()
    }
    pub fn from_bytes(bytes: [u8; 32]) -> PrivateIdentity {
        PrivateIdentity {
            lib: unsafe {
                // TODO: expose this from the `boring` crate
                let result = boring_sys::EVP_PKEY_new_raw_private_key(
                    boring_sys::EVP_PKEY_ED25519,
                    ptr::null_mut(),
                    bytes.as_ptr(),
                    bytes.len(),
                );
                assert!(!result.is_null());
                boring::pkey::PKey::from_ptr(result)
            },
        }
    }
    pub fn as_lib(&self) -> &boring::pkey::PKeyRef<boring::pkey::Private> {
        &self.lib
    }
    /// Vouches for a certificate this identity does not sit in, towards
    /// the client that sent the nonce: the identity and its signature.
    pub fn prove(&self, certificate_sha256: &[u8; 32], nonce: &[u8; 32]) -> [u8; IDENTITY_PROOF_SIZE] {
        let message = identity_proof_message(certificate_sha256, nonce);
        let mut signer = boring::sign::Signer::new_without_digest(&self.lib).unwrap();
        let signature = signer.sign_oneshot_to_vec(&message).unwrap();
        let mut proof = [0; IDENTITY_PROOF_SIZE];
        proof[..32].copy_from_slice(self.public().as_bytes());
        proof[32..].copy_from_slice(&signature);
        proof
    }
    /// A self-signed certificate holding the identity, valid from an
    /// hour before `now` for `IDENTITY_CERTIFICATE_LIFETIME`.
    pub fn generate_certificate(&self, now: i64) -> boring::x509::X509 {
        let name = {
            let mut builder = boring::x509::X509Name::builder().unwrap();
            builder
                .append_entry_by_nid(
                    boring::nid::Nid::ORGANIZATIONNAME,
                    "ddnet16-autogen",
                )
                .unwrap();
            builder
                .append_entry_by_nid(
                    boring::nid::Nid::COMMONNAME,
                    &format!("{}", self.public()),
                )
                .unwrap();
            builder.build()
        };
        // Ed25519 signs the message itself, without a digest; boringssl
        // wants that spelled as a null digest, which `MessageDigest` has
        // no safe constructor for.
        let default_md =
            unsafe { boring::hash::MessageDigest::from_ptr(ptr::null()) };

        let mut builder = boring::x509::X509::builder().unwrap();
        // TODO: what do we want to strip?
        builder.set_version(2).unwrap(); // 2 means version 3
        builder
            .set_serial_number(&random_bignum(160).to_asn1_integer().unwrap())
            .unwrap();
        builder.set_issuer_name(&name).unwrap();
        builder
            .set_not_before(&asn1_time(now - BROWSER_CERTIFICATE_BACKDATE))
            .unwrap();
        builder
            .set_not_after(&asn1_time(now + IDENTITY_CERTIFICATE_LIFETIME))
            .unwrap();
        builder.set_subject_name(&name).unwrap();
        builder.set_pubkey(&self.lib).unwrap();
        // TODO: find a way to set AKID, would need the first parameter of
        // x509_v3_context to be `Some(&builder)`, see apps/req.c:838 of
        // openssl (31157bc0b46e04227b8468d3e6915e4d0332777c).
        //builder.append_extension(boring::x509::extension::SubjectKeyIdentifier::new().build(&builder.x509v3_context(None, None)).unwrap()).unwrap();
        //builder.append_extension(boring::x509::extension::AuthorityKeyIdentifier::new().build(&builder.x509v3_context(None, None)).unwrap()).unwrap();
        builder
            .append_extension(
                boring::x509::extension::BasicConstraints::new()
                    .critical()
                    .ca()
                    .build()
                    .unwrap(),
            )
            .unwrap();
        builder.sign(&self.lib, default_md).unwrap();
        builder.build()
    }
}

#[cfg(all(test, not(target_os = "emscripten")))]
mod test {
    use super::BrowserCertificate;
    use super::Identity;
    use super::PrivateIdentity;
    use super::BROWSER_CERTIFICATE_LIFETIME;
    use super::BROWSER_CERTIFICATE_BACKDATE;
    use super::IDENTITY_CERTIFICATE_LIFETIME;

    #[test]
    fn browser_certificate_is_short_lived() {
        let cert = BrowserCertificate::generate(1_800_000_000);
        let leaf = &cert.chain()[0];
        let lifetime = leaf.not_before().diff(leaf.not_after()).unwrap();
        let seconds = i64::from(lifetime.days) * 24 * 60 * 60 + i64::from(lifetime.secs);
        assert_eq!(seconds, BROWSER_CERTIFICATE_LIFETIME + BROWSER_CERTIFICATE_BACKDATE);
        assert!(seconds <= 14 * 24 * 60 * 60);
        assert!(leaf.public_key().unwrap().public_eq(cert.key()));
        assert_eq!(cert.sha256(), leaf.digest(boring::hash::MessageDigest::sha256()).unwrap().as_ref());
    }

    #[test]
    fn identity_proof_round_trip() {
        let identity = PrivateIdentity::random();
        let other = PrivateIdentity::random();
        let sha256 = [7; 32];
        let nonce = [9; 32];
        let proof = identity.prove(&sha256, &nonce);
        assert!(identity.public().verify_proof(&proof, &sha256, &nonce).is_some());
        assert!(identity.public().verify_proof(&proof, &[8; 32], &nonce).is_none());
        assert!(identity.public().verify_proof(&proof, &sha256, &[0; 32]).is_none());
        assert!(other.public().verify_proof(&proof, &sha256, &nonce).is_none());
        assert!(identity.public().verify_proof(&proof[..95], &sha256, &nonce).is_none());
    }

    #[test]
    fn identity_from_hex() {
        let hex = "89b84bbc4b430a74642a8d6ee9086048318b20090e5a5d0c807aba4ce2c0d22f";
        let identity: Identity = hex.parse().unwrap();
        assert_eq!(identity.to_string(), hex);
        assert!(hex[..63].parse::<Identity>().is_err());
        assert!(format!("{}0", hex).parse::<Identity>().is_err());
        assert!(format!("+{}", &hex[1..]).parse::<Identity>().is_err());
        assert!(format!("{}g", &hex[..63]).parse::<Identity>().is_err());
        assert!(format!("{}ö", &hex[..62]).parse::<Identity>().is_err());
    }

    #[test]
    fn generate_certificate() {
        let private_identity: PrivateIdentity =
            "89b84bbc4b430a74642a8d6ee9086048318b20090e5a5d0c807aba4ce2c0d22f"
                .parse()
                .unwrap();
        let cert = private_identity.generate_certificate(1_800_000_000);
        cert.to_pem().unwrap();
        let lifetime = cert.not_before().diff(cert.not_after()).unwrap();
        let seconds = i64::from(lifetime.days) * 24 * 60 * 60 + i64::from(lifetime.secs);
        assert_eq!(seconds, IDENTITY_CERTIFICATE_LIFETIME + BROWSER_CERTIFICATE_BACKDATE);
        assert!(cert.public_key().unwrap().public_eq(private_identity.as_lib()));
    }
}
