/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2022 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen_netcore/pch.h"
#include "oxygen_netcore/network/ConnectionManager.h"
#include "oxygen_netcore/network/LowLevelPackets.h"
#include "oxygen_netcore/network/NetConnection.h"
#include "oxygen_netcore/network/internal/CryptoFunctions.h"


ConnectionManager::ConnectionManager(UDPSocket* udpSocket, TCPSocket* tcpListenSocket, ConnectionListenerInterface& listener, VersionRange<uint8> highLevelProtocolVersionRange) :
	mUDPSocket(udpSocket),
	mTCPListenSocket(tcpListenSocket),
	mListener(listener),
	mHighLevelProtocolVersionRange(highLevelProtocolVersionRange)
{
	mActiveConnections.reserve(8);
	mActiveConnectionsLookup.resize(8);
	mBitmaskForActiveConnectionsLookup = (uint16)(mActiveConnectionsLookup.size() - 1);
}

void ConnectionManager::updateConnections(uint64 currentTimestamp)
{
	for (NetConnection* connection : mActiveConnectionsLookup)
	{
		if (nullptr != connection)
		{
			// Update connection
			connection->updateConnection(currentTimestamp);
		}
	}
}

bool ConnectionManager::updateReceivePackets()
{
	bool anyActivity = false;

	// Update UDP
	if (nullptr != mUDPSocket)
	{
		for (int runs = 0; runs < 10; ++runs)
		{
			// Receive next packet
			static UDPSocket::ReceiveResult received;
			const bool success = mUDPSocket->receiveNonBlocking(received);
			if (!success)
			{
				// TODO: Handle error in socket
				return false;
			}

			if (received.mBuffer.empty())
			{
				// Nothing to do at the moment
				break;
			}

			anyActivity = true;
			receivedPacketInternal(received.mBuffer, received.mSenderAddress, nullptr);
		}
	}

	// Update TCP listen socket (server only)
	if (nullptr != mTCPListenSocket)
	{
		// Accept new connections
		for (int runs = 0; runs < 3; ++runs)
		{
			TCPSocket newSocket;
			if (!mTCPListenSocket->acceptConnection(newSocket))
				break;

			RMX_LOG_INFO("Accepted TCP connection");
			anyActivity = true;
			mIncomingTCPConnections.emplace_back();
			mIncomingTCPConnections.back().swapWith(newSocket);
		}
	}

	// Update net connections' TCP sockets
	for (NetConnection* connection : mTCPNetConnections)
	{
		// Receive next packet
		static TCPSocket::ReceiveResult received;
		const bool success = connection->mTCPSocket.receiveNonBlocking(received);
		if (!success)
		{
			// TODO: Handle error in socket
			return false;
		}

		if (!received.mBuffer.empty())
		{
			// Test for some very basic WebSocket support
		#if 0
			if (received.mBuffer.size() >= 0x80 && *(uint32*)&received.mBuffer[0] == *(uint32*)"GET ")
			{
				// Parse WebSocket handshake header
				String input;
				input.add((char*)&received.mBuffer[0], received.mBuffer.size());
				String webSocketKey;
				uint8 flags = 0;
				String line;
				int pos = 0;
				while (pos < input.length())
				{
					pos = input.getLine(line, pos);
					if (line == "Upgrade: websocket")
					{
						flags |= 0x01;
					}
					else if (line == "Connection: Upgrade")
					{
						flags |= 0x02;
					}
					else if (line.startsWith("Sec-WebSocket-Key: "))
					{
						line.remove(0, 19);
					#if 1
						webSocketKey = line;
						flags |= 0x04;
					#else
						std::vector<uint8> buffer;
						if (Crypto::decodeBase64(line.toStdString(), buffer))
						{
							webSocketKey.add((const char*)&buffer[0], buffer.size());
							flags |= 0x04;
						}
					#endif
					}
				}
				if ((flags & 0x07) == 0x07)
				{
					static const std::string GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
					uint32 sha1[5];
					Crypto::buildSHA1(webSocketKey.toStdString() + GUID, sha1);
					const std::string acceptString = Crypto::encodeBase64((const uint8*)sha1, sizeof(uint32) * 5);

					String str;
					str << "HTTP/1.1 101 Switching Protocols\r\n";
					str << "Upgrade: websocket\r\n";
					str << "Connection: Upgrade\r\n";
					str << "Sec-WebSocket-Accept: " << acceptString << "\r\n";
					str << "Sec-WebSocket-Protocol: oxygenserver\r\n";
					str << "\r\n";
					connection->mTCPSocket.sendData((const uint8*)str.getData(), str.length());
				}
			}
			else
		#endif
			{
				receivedPacketInternal(received.mBuffer, connection->getRemoteAddress(), connection);
			}
			anyActivity = true;
		}
	}

	return anyActivity;
}

void ConnectionManager::syncPacketQueues()
{
	// TODO: Lock mutex, so the worker thread stops briefly
	//  -> This is needed for the worker queue, and also for the "mReceivedPacketPool"

	// First sync queues
	{
		while (!mReceivedPackets.mWorkerQueue.empty())
		{
			mReceivedPackets.mSyncedQueue.push_back(mReceivedPackets.mWorkerQueue.front());
			mReceivedPackets.mWorkerQueue.pop_front();
		}
	}

	// Cleanup packets previously marked to be returned
	{
		for (const ReceivedPacket* receivedPacket : mReceivedPackets.mToBeReturned.mPackets)
		{
			mReceivedPacketPool.returnObject(*const_cast<ReceivedPacket*>(receivedPacket));
		}
		mReceivedPackets.mToBeReturned.mPackets.clear();
	}

	// TODO: Mutex can be unlocked here
}

ReceivedPacket* ConnectionManager::getNextReceivedPacket()
{
	if (mReceivedPackets.mSyncedQueue.empty())
		return nullptr;

	ReceivedPacket* receivedPacket = mReceivedPackets.mSyncedQueue.front();
	mReceivedPackets.mSyncedQueue.pop_front();

	// Packet initialization:
	//  - Set the "dump" instance that packets get returned to when the reference counter reaches zero
	//  - Start with a reference count of 1
	receivedPacket->initializeWithDump(&mReceivedPackets.mToBeReturned);
	return receivedPacket;
}

bool ConnectionManager::sendUDPPacketData(const std::vector<uint8>& data, const SocketAddress& remoteAddress)
{
#ifdef DEBUG
	// Simulate packet loss
	if (mDebugSettings.mSendingPacketLoss > 0.0f && randomf() < mDebugSettings.mSendingPacketLoss)
	{
		// Act as if the packet was sent successfully
		return true;
	}
#endif

	RMX_ASSERT(nullptr != mUDPSocket, "No UDP socket set");
	return mUDPSocket->sendData(data, remoteAddress);
}

bool ConnectionManager::sendTCPPacketData(const std::vector<uint8>& data, TCPSocket& socket)
{
#ifdef DEBUG
	// Simulate packet loss
	if (mDebugSettings.mSendingPacketLoss > 0.0f && randomf() < mDebugSettings.mSendingPacketLoss)
	{
		// Act as if the packet was sent successfully
		return true;
	}
#endif

	return socket.sendData(data);
}

bool ConnectionManager::sendConnectionlessLowLevelPacket(lowlevel::PacketBase& lowLevelPacket, const SocketAddress& remoteAddress, uint16 localConnectionID, uint16 remoteConnectionID)
{
	// Write low-level packet header
	static std::vector<uint8> sendBuffer;
	sendBuffer.clear();

	VectorBinarySerializer serializer(false, sendBuffer);
	serializer.write(lowLevelPacket.getSignature());
	serializer.write(localConnectionID);
	serializer.write(remoteConnectionID);
	lowLevelPacket.serializePacket(serializer, lowlevel::PacketBase::LOWLEVEL_PROTOCOL_VERSIONS.mMinimum);

	return sendUDPPacketData(sendBuffer, remoteAddress);
}

NetConnection* ConnectionManager::findConnectionTo(uint64 senderKey) const
{
	const auto it = mConnectionsBySender.find(senderKey);
	return (it == mConnectionsBySender.end()) ? nullptr : it->second;
}

void ConnectionManager::addConnection(NetConnection& connection)
{
	const uint16 localConnectionID = getFreeLocalConnectionID();
	RMX_CHECK(localConnectionID != 0, "Error in connection management: Could not assign a valid connection ID", return);

	connection.mLocalConnectionID = localConnectionID;
	mActiveConnections[localConnectionID] = &connection;
	mActiveConnectionsLookup[localConnectionID & mBitmaskForActiveConnectionsLookup] = &connection;
	mConnectionsBySender[connection.getSenderKey()] = &connection;

	if (connection.mUsingTCP)
	{
		mTCPNetConnections.push_back(&connection);
	}
}

void ConnectionManager::removeConnection(NetConnection& connection)
{
	// This method gets called from "NetConnection::clear", so it's safe to assume the connection gets cleaned up internally already
	mActiveConnections.erase(connection.getLocalConnectionID());
	mActiveConnectionsLookup[connection.getLocalConnectionID() & mBitmaskForActiveConnectionsLookup] = nullptr;
	mConnectionsBySender.erase(connection.getSenderKey());

	// TODO: Maybe reduce size of "mActiveConnectionsLookup" again if there's only few connections left - and if this does not produce any conflicts

	if (connection.mUsingTCP)
	{
		for (size_t index = 0; index < mTCPNetConnections.size(); ++index)
		{
			if (&connection == mTCPNetConnections[index])
			{
				mTCPNetConnections.erase(mTCPNetConnections.begin() + index);
				break;
			}
		}
	}
}

SentPacket& ConnectionManager::rentSentPacket()
{
	SentPacket& sentPacket = mSentPacketPool.rentObject();
	sentPacket.initializeWithPool(mSentPacketPool);
	return sentPacket;
}

void ConnectionManager::receivedPacketInternal(const std::vector<uint8>& buffer, const SocketAddress& senderAddress, NetConnection* connection)
{
	// Ignore too small packets
	if (buffer.size() < 6)
		return;

#ifdef DEBUG
	// Simulate packet loss
	if (mDebugSettings.mReceivingPacketLoss > 0.0f && randomf() < mDebugSettings.mReceivingPacketLoss)
		return;
#endif

	// Received a packet, check its signature
	VectorBinarySerializer serializer(true, buffer);
	const uint16 lowLevelSignature = serializer.read<uint16>();
	if (lowLevelSignature == lowlevel::StartConnectionPacket::SIGNATURE)
	{
		// Store for later evaluation
		ReceivedPacket& receivedPacket = mReceivedPacketPool.rentObject();
		receivedPacket.mContent = buffer;
		receivedPacket.mLowLevelSignature = lowLevelSignature;
		receivedPacket.mSenderAddress = senderAddress;
		receivedPacket.mConnection = connection;
		mReceivedPackets.mWorkerQueue.emplace_back(&receivedPacket);
	}
	else
	{
		// TODO: Explicitly check the known (= valid) signature types here?

		const uint16 remoteConnectionID = serializer.read<uint16>();
		const uint16 localConnectionID = serializer.read<uint16>();
		if (localConnectionID == 0)
		{
			// Invalid connection
			if (lowLevelSignature == lowlevel::ErrorPacket::SIGNATURE)
			{
				// TODO: Evaluate the error code
				RMX_ASSERT(false, "Received error packet");
			}
		}
		else
		{
			// We already know the connection for TCP, but not for UDP
			if (nullptr == connection)
			{
				// Find the connection in our list of active connections
				connection = mActiveConnectionsLookup[localConnectionID & mBitmaskForActiveConnectionsLookup];
			}

			if (nullptr == connection)
			{
				// Unknown connection
				if (lowLevelSignature == lowlevel::ErrorPacket::SIGNATURE)
				{
					// It's an error packet, don't send anything back
					// TODO: Evaluate the error code
				}
				else
				{
					// TODO: Send back an error packet - or better enqueue a notice to send one (if this method is executed by a thread)
				}
			}
			else
			{
				if (connection->getRemoteConnectionID() != remoteConnectionID && lowLevelSignature != lowlevel::AcceptConnectionPacket::SIGNATURE)
				{
					// Unknown or invalid connection
					// TODO: Send back an error packet - or better enqueue a notice to send one (if this method is executed by a thread)
				}
				else
				{
					// Store for later evaluation
					ReceivedPacket& receivedPacket = mReceivedPacketPool.rentObject();
					receivedPacket.mContent = buffer;
					receivedPacket.mLowLevelSignature = lowLevelSignature;
					receivedPacket.mSenderAddress = senderAddress;
					receivedPacket.mConnection = connection;
					mReceivedPackets.mWorkerQueue.emplace_back(&receivedPacket);
				}
			}
		}
	}
}

uint16 ConnectionManager::getFreeLocalConnectionID()
{
	// Make sure the lookup is always large enough (not filled by more than 75%)
	if (mActiveConnections.size() + 1 >= mActiveConnectionsLookup.size() * 3/4)
	{
		const size_t oldSize = mActiveConnectionsLookup.size();
		const size_t newSize = std::max<size_t>(mActiveConnectionsLookup.size() * 2, 32);
		mActiveConnectionsLookup.resize(newSize);
		mBitmaskForActiveConnectionsLookup = (uint16)(mActiveConnectionsLookup.size() - 1);

		// Move all entries whose index does not fit any more
		for (size_t k = 0; k < oldSize; ++k)
		{
			NetConnection* foundConnection = mActiveConnectionsLookup[k];
			if (nullptr != foundConnection)
			{
				const uint16 newIndex = foundConnection->getLocalConnectionID() & mBitmaskForActiveConnectionsLookup;
				if (newIndex != k)
				{
					mActiveConnectionsLookup[newIndex] = foundConnection;
					mActiveConnectionsLookup[k] = nullptr;
				}
			}
		}
	}

	// Get a new random local connection ID candidate
	static_assert(RAND_MAX >= 0xff);
	uint16 localConnectionID = (rand() & 0xff) + ((rand() & 0xff) << 8);
	for (size_t tries = 0; tries < mActiveConnectionsLookup.size(); ++tries)
	{
		// Exclude 0, as it would be an invalid connection ID
		if (localConnectionID != 0)
		{
			// Check if it's free in the lookup
			const uint16 index = localConnectionID & mBitmaskForActiveConnectionsLookup;
			if (nullptr == mActiveConnectionsLookup[index])
			{
				// We're good to go
				return localConnectionID;
			}
		}
		++localConnectionID;
	}
	return 0;
}
