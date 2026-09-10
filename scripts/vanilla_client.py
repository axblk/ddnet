#!/usr/bin/env python3
"""A Teeworlds 0.6 client without tokens, as far as the server's handshake.

The DDNet client always asks for a token, so the server's path for clients
that do not (the vanilla anti-spoof handshake, or the plain accept without
it) needs a client of its own to be tested. This one speaks just enough of
the 0.6 protocol for that: the control messages, chunked packets with
Huffman compression, and the packed ints of the system messages.

Usage: vanilla_client.py <host> <port> [password]
"""

import socket
import sys

# Packet flags, in the high nibble of the first header byte.
PACKETFLAG_CONTROL = 1
PACKETFLAG_CONNLESS = 2
PACKETFLAG_RESEND = 4
PACKETFLAG_COMPRESSION = 8

CTRLMSG_KEEPALIVE = 0
CTRLMSG_CONNECT = 1
CTRLMSG_CONNECTACCEPT = 2
CTRLMSG_ACCEPT = 3
CTRLMSG_CLOSE = 4

CHUNKFLAG_VITAL = 1
CHUNKFLAG_RESEND = 2

NETMSG_INFO = 1
NETMSG_MAP_CHANGE = 2
NETMSG_MAP_DATA = 3
NETMSG_CON_READY = 4
NETMSG_SNAPEMPTY = 6
NETMSG_READY = 14
NETMSG_INPUT = 16

NET_VERSION = "0.6 626fce9a778df4d4"

HUFFMAN_EOF_SYMBOL = 256
# `CHuffman::ms_aFreqTable`.
# fmt: off
HUFFMAN_FREQ_TABLE = [
	1 << 30, 4545, 2657, 431, 1950, 919, 444, 482, 2244, 617, 838, 542, 715, 1814, 304, 240, 754, 212, 647, 186,
	283, 131, 146, 166, 543, 164, 167, 136, 179, 859, 363, 113, 157, 154, 204, 108, 137, 180, 202, 176,
	872, 404, 168, 134, 151, 111, 113, 109, 120, 126, 129, 100, 41, 20, 16, 22, 18, 18, 17, 19,
	16, 37, 13, 21, 362, 166, 99, 78, 95, 88, 81, 70, 83, 284, 91, 187, 77, 68, 52, 68,
	59, 66, 61, 638, 71, 157, 50, 46, 69, 43, 11, 24, 13, 19, 10, 12, 12, 20, 14, 9,
	20, 20, 10, 10, 15, 15, 12, 12, 7, 19, 15, 14, 13, 18, 35, 19, 17, 14, 8, 5,
	15, 17, 9, 15, 14, 18, 8, 10, 2173, 134, 157, 68, 188, 60, 170, 60, 194, 62, 175, 71,
	148, 67, 167, 78, 211, 67, 156, 69, 1674, 90, 174, 53, 147, 89, 181, 51, 174, 63, 163, 80,
	167, 94, 128, 122, 223, 153, 218, 77, 200, 110, 190, 73, 174, 69, 145, 66, 277, 143, 141, 60,
	136, 53, 180, 57, 142, 57, 158, 61, 166, 112, 152, 92, 26, 22, 21, 28, 20, 26, 30, 21,
	32, 27, 20, 17, 23, 21, 30, 22, 22, 21, 27, 25, 17, 27, 23, 18, 39, 26, 15, 21,
	12, 18, 18, 27, 20, 18, 15, 19, 11, 17, 33, 12, 18, 15, 19, 18, 16, 26, 17, 18,
	9, 10, 25, 22, 22, 17, 20, 16, 6, 16, 15, 20, 14, 18, 24, 335, 1,
]
# fmt: on


class Huffman:
	"""`CHuffman`, tree and codes built the same way, without the lookup table."""

	def __init__(self):
		# A node: [num_bits, symbol, leaf0, leaf1, bits]; leaves are node
		# indices, -1 for none. Symbol nodes start with num_bits set, so the
		# code assignment can tell them from inner nodes.
		self.nodes = [[1, symbol, -1, -1, 0] for symbol in range(HUFFMAN_EOF_SYMBOL + 1)]
		left = [[frequency, node] for node, frequency in enumerate(HUFFMAN_FREQ_TABLE)]
		while len(left) > 1:
			# A stable sort by frequency, descending, like the original.
			left.sort(key=lambda entry: -entry[0])
			node = len(self.nodes)
			self.nodes.append([0, 0, left[-1][1], left[-2][1], 0])
			left[-2][1] = node
			left[-2][0] += left[-1][0]
			left.pop()
		self.start = len(self.nodes) - 1
		self._set_bits(self.start, 0, 0)

	def _set_bits(self, node, bits, depth):
		entry = self.nodes[node]
		if entry[3] != -1:
			self._set_bits(entry[3], bits | (1 << depth), depth + 1)
		if entry[2] != -1:
			self._set_bits(entry[2], bits, depth + 1)
		if entry[0]:
			entry[4] = bits
			entry[0] = depth

	def compress(self, data):
		out = bytearray()
		bits = 0
		bitcount = 0
		for symbol in list(data) + [HUFFMAN_EOF_SYMBOL]:
			entry = self.nodes[symbol]
			bits |= entry[4] << bitcount
			bitcount += entry[0]
			while bitcount >= 8:
				out.append(bits & 0xFF)
				bits >>= 8
				bitcount -= 8
		if bitcount:
			out.append(bits & 0xFF)
		return bytes(out)

	def decompress(self, data):
		out = bytearray()
		bits = 0
		bitcount = 0
		position = 0
		while True:
			while bitcount < 24 and position < len(data):
				bits |= data[position] << bitcount
				bitcount += 8
				position += 1
			node = self.start
			while not self.nodes[node][0]:
				if bitcount == 0:
					raise ValueError("huffman data ends inside a symbol")
				node = self.nodes[node][3] if bits & 1 else self.nodes[node][2]
				bits >>= 1
				bitcount -= 1
			if node == HUFFMAN_EOF_SYMBOL:
				return bytes(out)
			out.append(self.nodes[node][1])


HUFFMAN = Huffman()


def pack_int(value):
	"""`CVariableInt::Pack`."""
	out = bytearray()
	byte = (value >> 25) & 0x40
	if value < 0:
		value = ~value
	byte |= value & 0x3F
	value >>= 6
	while value:
		out.append(byte | 0x80)
		byte = value & 0x7F
		value >>= 7
	out.append(byte)
	return bytes(out)


def unpack_int(data, position):
	"""`CVariableInt::Unpack`; returns the value and the position after it."""
	first = data[position]
	sign = (first >> 6) & 1
	value = first & 0x3F
	position += 1
	masks = [0x7F, 0x7F, 0x7F, 0x0F]
	shifts = [6, 6 + 7, 6 + 7 + 7, 6 + 7 + 7 + 7]
	more = first & 0x80
	for mask, shift in zip(masks, shifts):
		if not more:
			break
		byte = data[position]
		position += 1
		value |= (byte & mask) << shift
		more = byte & 0x80
	if sign:
		value = ~value
	return value, position


def unpack_string(data, position):
	end = data.index(0, position)
	return data[position:end].decode("utf-8", "replace"), end + 1


def header(flags, ack, num_chunks):
	return bytes([((flags << 4) & 0xF0) | ((ack >> 8) & 0xF), ack & 0xFF, num_chunks])


def control_packet(msg, ack=0, extra=b""):
	return header(PACKETFLAG_CONTROL, ack, 0) + bytes([msg]) + extra


def chunk(data, vital_sequence=None):
	size = len(data)
	flags = CHUNKFLAG_VITAL if vital_sequence is not None else 0
	out = bytearray([(flags << 6) | ((size >> 4) & 0x3F), size & 0xF])
	if vital_sequence is not None:
		out[1] |= (vital_sequence >> 2) & 0xF0
		out.append(vital_sequence & 0xFF)
	return bytes(out) + data


def data_packet(ack, chunks):
	"""A packet of chunks, compressed when that is smaller, like the client sends them."""
	payload = b"".join(chunks)
	compressed = HUFFMAN.compress(payload)
	if len(compressed) < len(payload):
		return header(PACKETFLAG_COMPRESSION, ack, len(chunks)) + compressed
	return header(0, ack, len(chunks)) + payload


class Packet:
	def __init__(self, raw):
		self.flags = raw[0] >> 4
		self.ack = ((raw[0] & 0xF) << 8) | raw[1]
		self.num_chunks = raw[2]
		payload = raw[3:]
		self.control = None
		self.chunks = []
		if self.flags & PACKETFLAG_CONNLESS:
			return
		if self.flags & PACKETFLAG_COMPRESSION:
			payload = HUFFMAN.decompress(payload)
		if self.flags & PACKETFLAG_CONTROL:
			self.control = payload[0]
			self.control_data = payload[1:]
			return
		position = 0
		for _ in range(self.num_chunks):
			flags = payload[position] >> 6
			size = ((payload[position] & 0x3F) << 4) | (payload[position + 1] & 0xF)
			sequence = None
			if flags & CHUNKFLAG_VITAL:
				sequence = ((payload[position + 1] & 0xF0) << 2) | payload[position + 2]
				position += 3
			else:
				position += 2
			self.chunks.append((sequence, payload[position : position + size]))
			position += size


class VanillaClient:
	"""Connects like a client without tokens; `connect` returns the name of the
	map the server tells it to load once it is taken, by the anti-spoof
	handshake or straight away."""

	def __init__(self, host, port, timeout=5.0):
		self.addr = (host, port)
		self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
		self.sock.settimeout(timeout)
		self.sequence = 0
		self.ack = 0
		self.handshake_token = None

	def send(self, raw):
		self.sock.sendto(raw, self.addr)

	def recv(self):
		raw, _ = self.sock.recvfrom(2048)
		packet = Packet(raw)
		for sequence, _ in packet.chunks:
			if sequence is not None:
				self.ack = sequence
		return packet

	def next_sequence(self):
		self.sequence = (self.sequence + 1) % 1024
		return self.sequence

	def system_message(self, msg, *parts):
		data = pack_int((msg << 1) | 1)
		for part in parts:
			if isinstance(part, int):
				data += pack_int(part)
			else:
				data += part.encode() + b"\0"
		return data

	def connect(self, password=""):
		self.send(control_packet(CTRLMSG_CONNECT))
		packet = self.recv()
		if packet.control != CTRLMSG_CONNECTACCEPT:
			raise AssertionError(f"expected connect-accept, got {packet.control!r}")
		# With the handshake the map change and the snapshots follow at
		# once; without it nothing does until the client says who it is.
		self.sock.settimeout(1.0)
		try:
			packet = self.recv()
		except socket.timeout:
			packet = None
		self.sock.settimeout(5.0)
		if packet is not None and any(self.message_id(data) == NETMSG_SNAPEMPTY for _, data in packet.chunks):
			for _, data in packet.chunks:
				if self.message_id(data) == NETMSG_SNAPEMPTY:
					_, position = unpack_int(data, 0)
					self.handshake_token, _ = unpack_int(data, position)
			# The input acknowledges the snapshot's tick: the token.
			self.send(data_packet(self.ack, [chunk(self.system_message(NETMSG_INPUT, self.handshake_token, self.handshake_token + 1, 0))]))
		else:
			self.send(data_packet(self.ack, [chunk(self.system_message(NETMSG_INFO, NET_VERSION, password), self.next_sequence())]))
		for _ in range(10):
			packet = self.recv()
			if packet.control == CTRLMSG_CLOSE:
				reason = packet.control_data.split(b"\0", 1)[0].decode("utf-8", "replace")
				raise AssertionError(f"closed by the server: {reason!r}")
			for _, data in packet.chunks:
				if self.message_id(data) == NETMSG_MAP_CHANGE:
					name, _ = unpack_string(data, unpack_int(data, 0)[1])
					return name
		raise AssertionError("no map change from the server")

	@staticmethod
	def message_id(data):
		msg, _ = unpack_int(data, 0)
		if msg & 1 == 0:
			return None
		return msg >> 1

	def close(self, reason=""):
		self.send(control_packet(CTRLMSG_CLOSE, self.ack, reason.encode() + b"\0"))
		self.sock.close()


def main():
	host, port = sys.argv[1], int(sys.argv[2])
	password = sys.argv[3] if len(sys.argv) > 3 else ""
	client = VanillaClient(host, port)
	print(f"map: {client.connect(password)}")
	if client.handshake_token is not None:
		print(f"handshake token: {client.handshake_token}")
	client.close()


if __name__ == "__main__":
	main()
