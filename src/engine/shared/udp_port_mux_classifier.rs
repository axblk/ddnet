//! Pure datagram classification for a UDP port shared by legacy DDNet and QUIC.

pub(crate) const LEGACY_MAX_PACKET_SIZE: usize = 1400;
const LEGACY_PACKET_HEADER_SIZE: usize = 3;
const SIXUP_PACKET_HEADER_SIZE: usize = 7;
const SIXUP_CONNLESS_HEADER_SIZE: usize = 9;
pub(crate) const LEGACY_CONNLESS_HEADER_SIZE: usize = 6;
const MAX_QUIC_CID_SIZE: usize = 20;
pub(crate) const QUIC_CID_LEN: usize = 8;

const LEGACY_FLAG_UNUSED: u8 = 1 << 0;
const LEGACY_FLAG_CONTROL: u8 = 1 << 2;
const LEGACY_FLAG_CONNLESS: u8 = 1 << 3;
const LEGACY_FLAG_RESEND: u8 = 1 << 4;
const LEGACY_FLAG_COMPRESSION: u8 = 1 << 5;

const SIXUP_FLAG_CONTROL: u8 = 1 << 0;
const SIXUP_FLAG_RESEND: u8 = 1 << 1;
const SIXUP_FLAG_COMPRESSION: u8 = 1 << 2;

pub(crate) const QUIC_FIXED_BIT: u8 = 0x40;
const QUIC_LONG_HEADER_BIT: u8 = 0x80;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
/// The sole consumer selected for one datagram on the shared UDP port.
pub enum DatagramRoute {
    /// A stateless 0.6/0.7 server-info, master or LAN datagram.
    Connectionless,
    /// A connection-oriented legacy DDNet datagram.
    Legacy,
    /// A QUIC long-header packet or a short-header packet with a valid local CID.
    Quic,
    /// A malformed, unsupported or ambiguous datagram.
    Drop,
}

/// Selects exactly one consumer for a datagram received on the shared UDP port.
///
/// `known_legacy_peer` must only return true for an established legacy source
/// address. It is evaluated lazily after the connectionless fast path. QUIC
/// path migration is recognized by its destination connection ID and must not
/// set this flag.
/// `quic_cid_len` and `validate_quic_cid` must use the same secret and format as
/// the Quinn endpoint's connection ID generator.
pub fn classify(
    datagram: &[u8],
    known_legacy_peer: impl FnOnce() -> bool,
    quic_cid_len: usize,
    validate_quic_cid: impl Fn(&[u8]) -> bool,
) -> DatagramRoute {
    if is_connectionless(datagram) {
        return DatagramRoute::Connectionless;
    }
    let Some(&first) = datagram.first() else {
        return DatagramRoute::Drop;
    };
    if first & QUIC_LONG_HEADER_BIT != 0 && is_quic_long_header(datagram) {
        return DatagramRoute::Quic;
    }
    let valid_short_cid = first & QUIC_FIXED_BIT != 0
        && quic_cid_len != 0
        && quic_cid_len <= MAX_QUIC_CID_SIZE
        && datagram.len() >= 1 + quic_cid_len
        && validate_quic_cid(&datagram[1..1 + quic_cid_len]);
    if valid_short_cid {
        return DatagramRoute::Quic;
    }
    if known_legacy_peer() {
        // C++ owns the established slot and therefore also the authoritative
        // 0.6/0.7 flag interpretation. Re-parsing flags here would reject
        // valid 0.7 resend and compressed packets whose bits overlap 0.6.
        return if (LEGACY_PACKET_HEADER_SIZE..=LEGACY_MAX_PACKET_SIZE).contains(&datagram.len()) {
            DatagramRoute::Legacy
        } else {
            DatagramRoute::Drop
        };
    }
    if first & QUIC_FIXED_BIT != 0 {
        return DatagramRoute::Drop;
    }
    if is_legacy_packet(datagram) {
        DatagramRoute::Legacy
    } else {
        DatagramRoute::Drop
    }
}

fn is_quic_long_header(datagram: &[u8]) -> bool {
    // Only parse the version-independent header here. Quinn must see unknown
    // versions so it can perform Version Negotiation, and remains responsible
    // for parsing and authenticating the complete packet.
    if datagram.len() < 7 || datagram[0] & (QUIC_LONG_HEADER_BIT | QUIC_FIXED_BIT) != 0xc0 {
        return false;
    }
    let destination_len = datagram[5] as usize;
    if destination_len > MAX_QUIC_CID_SIZE || datagram.len() < 7 + destination_len {
        return false;
    }
    let source_len_offset = 6 + destination_len;
    let source_len = datagram[source_len_offset] as usize;
    source_len <= MAX_QUIC_CID_SIZE && datagram.len() > source_len_offset + 1 + source_len
}

fn is_connectionless(datagram: &[u8]) -> bool {
    if datagram.len() > LEGACY_MAX_PACKET_SIZE {
        return false;
    }
    datagram.len() >= LEGACY_CONNLESS_HEADER_SIZE
        && (datagram[..LEGACY_CONNLESS_HEADER_SIZE] == [0xff; LEGACY_CONNLESS_HEADER_SIZE]
            || datagram[..2] == *b"xe")
        || datagram.len() >= SIXUP_CONNLESS_HEADER_SIZE
            && datagram[0] == (LEGACY_FLAG_CONNLESS << 2) | 1
}

fn is_legacy_packet(datagram: &[u8]) -> bool {
    if !(LEGACY_PACKET_HEADER_SIZE..=LEGACY_MAX_PACKET_SIZE).contains(&datagram.len()) {
        return false;
    }
    let raw_flags = datagram[0] >> 2;
    let sixup = raw_flags & LEGACY_FLAG_UNUSED != 0;
    let (allowed_flags, control_flag, resend_flag, compression_flag, data_start) = if sixup {
        (
            SIXUP_FLAG_CONTROL | SIXUP_FLAG_RESEND | SIXUP_FLAG_COMPRESSION,
            SIXUP_FLAG_CONTROL,
            SIXUP_FLAG_RESEND,
            SIXUP_FLAG_COMPRESSION,
            SIXUP_PACKET_HEADER_SIZE,
        )
    } else {
        (
            LEGACY_FLAG_CONTROL | LEGACY_FLAG_RESEND | LEGACY_FLAG_COMPRESSION,
            LEGACY_FLAG_CONTROL,
            LEGACY_FLAG_RESEND,
            LEGACY_FLAG_COMPRESSION,
            LEGACY_PACKET_HEADER_SIZE,
        )
    };
    if raw_flags & !allowed_flags != 0 || datagram.len() < data_start {
        return false;
    }
    let chunks = datagram[2];
    let data_size = datagram.len() - data_start;
    if raw_flags & control_flag != 0 {
        chunks == 0 && data_size > 0 && raw_flags & compression_flag == 0
    } else {
        chunks > 0 || raw_flags & resend_flag != 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classifies_legacy_quic_collisions_without_dual_delivery() {
        const CID: [u8; QUIC_CID_LEN] = [1, 2, 3, 4, 5, 6, 7, 8];
        const ROTATED_CID: [u8; QUIC_CID_LEN] = [8, 7, 6, 5, 4, 3, 2, 1];
        let classify = |datagram: &[u8], known_legacy_peer| {
            super::classify(datagram, || known_legacy_peer, CID.len(), |cid| cid == CID)
        };

        assert_eq!(classify(&[0xff; 6], false), DatagramRoute::Connectionless);
        assert_eq!(
            classify(b"xe\0\0\0\0", false),
            DatagramRoute::Connectionless
        );
        assert_eq!(
            classify(&[0x21, 0, 0, 0, 0, 0, 0, 0, 0], false),
            DatagramRoute::Connectionless
        );
        assert_eq!(classify(&[0x40, 0, 0], true), DatagramRoute::Legacy);
        assert_eq!(classify(&[0x40, 0, 0], false), DatagramRoute::Drop);
        assert_eq!(classify(&[0xc0, 0, 0], true), DatagramRoute::Legacy);
        assert_eq!(classify(&[0xc0, 0, 0], false), DatagramRoute::Drop);

        let mut short = vec![QUIC_FIXED_BIT];
        short.extend_from_slice(&CID);
        short.push(0);
        assert_eq!(classify(&short, false), DatagramRoute::Quic);
        short[1] ^= 1;
        assert_eq!(classify(&short, false), DatagramRoute::Drop);
        short[1] ^= 1;

        let classify_rotated = |datagram: &[u8]| {
            super::classify(
                datagram,
                || true,
                CID.len(),
                |cid| cid == CID || cid == ROTATED_CID,
            )
        };
        let mut rotated_short = vec![QUIC_FIXED_BIT];
        rotated_short.extend_from_slice(&ROTATED_CID);
        rotated_short.push(0);
        assert_eq!(classify_rotated(&short), DatagramRoute::Quic);
        assert_eq!(classify_rotated(&rotated_short), DatagramRoute::Quic);
        assert_eq!(
            super::classify(&short, || false, CID.len(), |cid| cid == ROTATED_CID),
            DatagramRoute::Drop
        );
        assert_eq!(
            super::classify(&short, || true, CID.len(), |cid| cid == ROTATED_CID),
            DatagramRoute::Legacy
        );

        let seven_resend = [0x08, 0, 0, 0, 0, 0, 0];
        let seven_compressed = [0x10, 0, 1, 0, 0, 0, 0, 1];
        assert_eq!(classify(&seven_resend, true), DatagramRoute::Legacy);
        assert_eq!(classify(&seven_compressed, true), DatagramRoute::Legacy);
        assert_eq!(classify(&seven_resend, false), DatagramRoute::Drop);
        assert_eq!(classify(&seven_compressed, false), DatagramRoute::Drop);

        let mut initial = vec![0xc0, 0, 0, 0, 1, 8];
        initial.extend_from_slice(&[1; 8]);
        initial.push(8);
        initial.extend_from_slice(&[2; 8]);
        initial.extend_from_slice(&[0, 1, 0]);
        assert_eq!(classify(&initial, false), DatagramRoute::Quic);
        assert_eq!(classify(&initial, true), DatagramRoute::Quic);
        initial[4] = 2;
        assert_eq!(classify(&initial, false), DatagramRoute::Quic);
        initial[5] = 21;
        assert_eq!(classify(&initial, false), DatagramRoute::Drop);
        assert_eq!(
            classify(&[0xc0, 0, 0, 0, 1, 0, 0], false),
            DatagramRoute::Drop
        );

        for first in [0xe0, 0xf0] {
            let mut handshake_or_retry = vec![first, 0, 0, 0, 1, 8];
            handshake_or_retry.extend_from_slice(&CID);
            handshake_or_retry.push(8);
            handshake_or_retry.extend_from_slice(&ROTATED_CID);
            handshake_or_retry.push(0);
            assert_eq!(classify(&handshake_or_retry, true), DatagramRoute::Quic);
        }

        assert_eq!(classify(&[0x10, 0, 0, 1], false), DatagramRoute::Legacy);
        assert_eq!(classify(&[], false), DatagramRoute::Drop);
    }
}
