#include "reliable_connection.h"
#include <network_packet.h>
#include <iostream>
#include <chrono>
#include <halley/utils/utils.h>
#include <halley/support/exception.h>

using namespace Halley;

struct ReliableHeader
{
	unsigned short sequence = 0xFFFF;
	unsigned short ack = 0xFFFF;
	unsigned int ackBits = 0xFFFFFFFF;
};

struct ReliableSubHeader
{
	unsigned char sizeA;
	unsigned char sizeB;
	unsigned short resend;
};

constexpr size_t BUFFER_SIZE = 1024;

ReliableConnection::ReliableConnection(std::shared_ptr<IConnection> parent)
	: parent(parent)
	, receivedSeqs(BUFFER_SIZE)
	, sentPackets(BUFFER_SIZE)
{
	lastSend = lastReceive = Clock::now();
}

void ReliableConnection::close()
{
	parent->close();
}

ConnectionStatus ReliableConnection::getStatus() const
{
	return parent->getStatus();
}

void ReliableConnection::send(OutboundNetworkPacket&& packet)
{
	//throw Exception("Operation not supported.");
	sendTagged(std::move(packet), 0);
}

void ReliableConnection::sendTagged(OutboundNetworkPacket&& packet, int tag)
{
	Expects(tag >= 0);

	// Add reliable sub-header
	bool isResend = false;
	unsigned short resending = 0;
	size_t size = packet.getSize();
	bool longSize = size >= 64;
	ReliableSubHeader subHeader;
	subHeader.sizeA = static_cast<unsigned char>(longSize ? (size >> 8) & 0x3F : size) | (isResend ? 0x80 : 0);
	subHeader.sizeB = static_cast<unsigned char>(longSize ? (size & 0xFF) : 0);
	subHeader.resend = resending;
	gsl::span<ReliableSubHeader> subData(&subHeader, sizeof(ReliableSubHeader));
	packet.addHeader(subData.subspan(0, (longSize ? 2 : 1) + (isResend ? 2 : 0)));

	// Add reliable header
	ReliableHeader header;
	header.sequence = sequenceSent++;
	header.ack = highestReceived;
	header.ackBits = generateAckBits();
	packet.addHeader(gsl::span<ReliableHeader>(&header, sizeof(ReliableHeader)));
	parent->send(std::move(packet));

	// Store send information
	size_t idx = header.sequence % BUFFER_SIZE;
	auto& sent = sentPackets[idx];
	sent.waiting = true;
	sent.tag = tag;
	lastSend = sent.timestamp = Clock::now();
}

bool ReliableConnection::receive(InboundNetworkPacket& packet)
{
	// Process all incoming
	InboundNetworkPacket tmp;
	while (parent->receive(tmp)) {
		lastReceive = Clock::now();
		processReceivedPacket(tmp);
	}

	if (!pendingPackets.empty()) {
		packet = std::move(pendingPackets.front());
		pendingPackets.pop_front();
		return true;
	}

	return false;
}

void ReliableConnection::addAckListener(IReliableConnectionAckListener& listener)
{
	ackListeners.push_back(&listener);
}

void ReliableConnection::removeAckListener(IReliableConnectionAckListener& listener)
{
	ackListeners.erase(std::find(ackListeners.begin(), ackListeners.end(), &listener));
}

void ReliableConnection::processReceivedPacket(InboundNetworkPacket& packet)
{
	ReliableHeader header;
	packet.extractHeader(gsl::span<ReliableHeader>(&header, sizeof(ReliableHeader)));
	processReceivedAcks(header.ack, header.ackBits);
	unsigned short seq = header.sequence;

	while (packet.getSize() > 0) {
		if (packet.getSize() < 1) {
			throw Exception("Sub-packet header not found.");
		}

		// Sub-packets header
		ReliableSubHeader subHeader;
		gsl::span<ReliableSubHeader> data(&subHeader, sizeof(ReliableSubHeader));
		packet.extractHeader(data.subspan(0, 1));
		size_t size = 0;
		bool resend = (subHeader.sizeA & 0x80) != 0;
		if (subHeader.sizeA & 0x40) {
			if (packet.getSize() < 1) {
				throw Exception("Sub-packet header incomplete.");
			}
			packet.extractHeader(data.subspan(1, 1));
			size = (subHeader.sizeA & 0x3F) << 8 | (subHeader.sizeB);
		} else {
			size = subHeader.sizeA & 0x3F;
		}
		unsigned short resendOf = 0;
		if (resend) {
			if (packet.getSize() < 2) {
				throw Exception("Sub-packet header missing resend data");
			}
			packet.extractHeader(gsl::span<unsigned short>(&resendOf, 2));
		}

		// Extract data
		std::array<char, 2048> buffer;
		if (size > buffer.size() || size > packet.getSize()) {
			throw Exception("Unexpected sub-packet size");
		}
		auto subPacketData = gsl::span<char, 2048>(buffer).subspan(0, size);
		packet.extractHeader(subPacketData);

		// Process sub-packet
		if (onSeqReceived(seq, resend, resendOf)) {
			pendingPackets.push_back(InboundNetworkPacket(subPacketData));
		}
		++seq;
	}
}

void ReliableConnection::processReceivedAcks(unsigned short ack, unsigned int ackBits)
{
	// If acking something too far back in the past, ignore it
	unsigned short diff = sequenceSent - ack;
	if (diff > 512) {
		return;
	}

	for (int i = 32; --i >= 0; ) {
		if (ackBits & (1 << i)) {
			unsigned short seq = static_cast<unsigned short>(ack - (i + 1));
			onAckReceived(seq);
		}
	}
	onAckReceived(ack);
}

bool ReliableConnection::onSeqReceived(unsigned short seq, bool isResend, unsigned short resendOf)
{
	size_t bufferPos = size_t(seq) % BUFFER_SIZE;
	size_t resendPos = size_t(resendOf) % BUFFER_SIZE;
	unsigned short diff = seq - highestReceived;

	if (diff != 0 && diff < 0x8000) { // seq higher than highestReceived, with unsigned wrap-around
		if (diff > BUFFER_SIZE - 32) {
			// Ops, skipped too many packets!
			close();
			return false;
		}

		// Clear all packets half-buffer seqs ago (since the last cleared one)
		for (size_t i = highestReceived % BUFFER_SIZE; i != bufferPos; i = (i + 1) % BUFFER_SIZE) {
			size_t idx = (i + BUFFER_SIZE / 2) % BUFFER_SIZE;
			receivedSeqs[idx] = 0;
		}

		highestReceived = seq;
	}

	if (receivedSeqs[bufferPos] != 0 || (isResend && receivedSeqs[resendPos] != 0)) {
		// Already received
		return false;
	}

	// Mark this packet as received
	receivedSeqs[bufferPos] |= 1;
	if (isResend) {
		receivedSeqs[resendPos] |= 2;
	}

	return true;
}

void ReliableConnection::onAckReceived(unsigned short sequence)
{
	auto& data = sentPackets[sequence % BUFFER_SIZE];
	if (data.waiting) {
		data.waiting = false;
		if (data.tag != -1) {
			for (auto& listener : ackListeners) {
				listener->onPacketAcked(data.tag);
			}
		}
		float msgLag = std::chrono::duration<float>(Clock::now() - data.timestamp).count();
		reportLatency(msgLag);
	}
}

unsigned int ReliableConnection::generateAckBits()
{
	unsigned int result = 0;
	
	for (size_t i = 0; i < 32; i++) {
		size_t bufferPos = ((highestReceived - 1 - i) + 0x10000) % BUFFER_SIZE;
		result |= static_cast<unsigned int>(1 & receivedSeqs[bufferPos]) << i;
	}

	return result;
}

void ReliableConnection::reportLatency(float lastMeasuredLag)
{
	if (fabs(lag) < 0.00001f) {
		lag = lastMeasuredLag;
	} else {
		lag = lerp(lag, lastMeasuredLag, 0.2f);
	}
}

float ReliableConnection::getTimeSinceLastSend() const
{
	return std::chrono::duration<float>(Clock::now() - lastSend).count();
}

float ReliableConnection::getTimeSinceLastReceive() const
{
	return std::chrono::duration<float>(Clock::now() - lastReceive).count();
}

