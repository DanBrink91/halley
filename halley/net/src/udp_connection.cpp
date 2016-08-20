#include "udp_connection.h"
#include <network_packet.h>
#include <iostream>

using namespace Halley;



struct HandshakeAccept
{
	HandshakeAccept()
		: handshake("halley_accp")
	{
	}

	const char handshake[12];
	short id = -1;
	// TODO: encrypted session key
};



UDPConnection::UDPConnection(UDPSocket& socket, UDPEndpoint remote)
	: socket(socket)
	, remote(remote)
	, status(ConnectionStatus::CONNECTING)
	, connectionId(-1)
{
}

void UDPConnection::close()
{
	onClose();
	status = ConnectionStatus::CLOSING;
}

void UDPConnection::terminateConnection()
{
	onClose();
	status = ConnectionStatus::CLOSED;
}

void UDPConnection::onClose()
{
	if (status == ConnectionStatus::OPEN) {
		// TODO: send close connection message
	}
}

void UDPConnection::send(OutboundNetworkPacket&& packet)
{
	if (status == ConnectionStatus::OPEN || status == ConnectionStatus::CONNECTING) {
		// Insert header
		std::array<char, 1> id = { -1 };
		packet.addHeader(gsl::span<char>(id));

		bool needsSend = pendingSend.empty();
		pendingSend.emplace_back(std::move(packet));
		if (needsSend) {
			sendNext();
		}
	}
}

bool UDPConnection::receive(InboundNetworkPacket& packet)
{
	if (pendingReceive.empty()) {
		return false;
	} else {
		packet = std::move(pendingReceive.front());
		pendingReceive.pop_front();
		return true;
	}
}

bool UDPConnection::matchesEndpoint(short id, const UDPEndpoint& remoteEndpoint) const
{
	return (id == -1 || id == connectionId) && remote == remoteEndpoint;
}

void UDPConnection::onReceive(gsl::span<const gsl::byte> data)
{
	Expects(data.size() <= 1500);

	if (status == ConnectionStatus::CONNECTING) {
		HandshakeAccept accept;

		if (data.size_bytes() == sizeof(accept)) {
			if (memcmp(data.data(), &accept, sizeof(accept.handshake)) == 0) {
				onOpen(accept.id);
			}
		}
	} else if (status == ConnectionStatus::OPEN) {
		if (data.size() <= 1500) {
			pendingReceive.push_back(InboundNetworkPacket(data));
		}
	}
}

void UDPConnection::setError(const std::string& cs)
{
	error = cs;
}

void UDPConnection::open(short id)
{
	if (status == ConnectionStatus::CONNECTING) {
		// Handshake
		HandshakeAccept accept;
		accept.id = id;
		send(OutboundNetworkPacket(gsl::span<HandshakeAccept>(&accept, sizeof(HandshakeAccept))));

		onOpen(id);
	}
}

void UDPConnection::onOpen(short id)
{
	std::cout << "Connection open on id = " << id << std::endl;
	connectionId = id;
	status = ConnectionStatus::OPEN;
}

void UDPConnection::sendNext()
{
	if (pendingSend.empty()) {
		return;
	}

	auto& packet = pendingSend.front();

	// Append header
	//packet.addHeader()

	size_t size = packet.copyTo(sendBuffer);
	pendingSend.pop_front();

	socket.async_send_to(boost::asio::buffer(sendBuffer, size), remote, [this] (const boost::system::error_code& error, std::size_t size)
	{
		//std::cout << "Sent " << size << " bytes\n";
		if (error) {
			std::cout << "Error sending packet: " << error.message() << std::endl;
			close();
		} else if (!pendingSend.empty()) {
			sendNext();
		}
	});
}
