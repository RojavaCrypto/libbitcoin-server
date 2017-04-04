/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/server/workers/query_worker.hpp>

#include <functional>
#include <string>
#include <bitcoin/protocol.hpp>
#include <bitcoin/server/define.hpp>
#include <bitcoin/server/interface/address.hpp>
#include <bitcoin/server/interface/blockchain.hpp>
#include <bitcoin/server/interface/protocol.hpp>
#include <bitcoin/server/interface/transaction_pool.hpp>
#include <bitcoin/server/messages/message.hpp>
#include <bitcoin/server/server_node.hpp>

namespace libbitcoin {
namespace server {

using namespace std::placeholders;
using namespace bc::protocol;

query_worker::query_worker(zmq::authenticator& authenticator,
    server_node& node, bool secure)
  : worker(priority(node.server_settings().priority)),
    secure_(secure),
    verbose_(node.network_settings().verbose),
    settings_(node.server_settings()),
    node_(node),
    authenticator_(authenticator)
{
    // The same interface is attached to the secure and public interfaces.
    attach_interface();
}

// Implement worker as a dealer to the query service.
// v2 libbitcoin-client DEALER does not add delimiter frame.
// The dealer drops messages for lost peers (query service) and high water.
void query_worker::work()
{
    // Use a dealer for this synchronous response because notifications are
    // sent asynchronously to the same identity via the same dealer. Using a
    // router is okay but it adds an additional address to the envelope that
    // would have to be stripped by the notification dealer so this is simpler.
    zmq::socket dealer(authenticator_, zmq::socket::role::dealer);

    // Connect socket to the service endpoint.
    if (!started(connect(dealer)))
        return;

    zmq::poller poller;
    poller.add(dealer);

    while (!poller.terminated() && !stopped())
    {
        if (poller.wait().contains(dealer.id()))
            query(dealer);
    }

    // Disconnect the socket and exit this thread.
    finished(disconnect(dealer));
}

// Connect/Disconnect.
//-----------------------------------------------------------------------------

bool query_worker::connect(zmq::socket& dealer)
{
    const auto security = secure_ ? "secure" : "public";
    const auto& endpoint = secure_ ? query_service::secure_worker :
        query_service::public_worker;

    const auto ec = dealer.connect(endpoint);

    if (ec)
    {
        LOG_ERROR(LOG_SERVER)
            << "Failed to connect " << security << " query worker to "
            << endpoint << " : " << ec.message();
        return false;
    }

    LOG_INFO(LOG_SERVER)
        << "Connected " << security << " query worker to " << endpoint;
    return true;
}

bool query_worker::disconnect(zmq::socket& dealer)
{
    const auto security = secure_ ? "secure" : "public";

    // Don't log stop success.
    if (dealer.stop())
        return true;

    LOG_ERROR(LOG_SERVER)
        << "Failed to disconnect " << security << " query worker.";
    return false;
}

// Query Execution.
// The dealer send blocks until the query service dealer is available.
//-----------------------------------------------------------------------------

// private/static
void query_worker::send(const message& response, zmq::socket& dealer)
{
    const auto ec = response.send(dealer);

    if (ec && ec != error::service_stopped)
        LOG_WARNING(LOG_SERVER)
            << "Failed to send query response to "
            << response.route().display() << " " << ec.message();
}

// Because the socket is a router we may simply drop invalid queries.
// As a single thread worker this router should not reach high water.
// If we implemented as a replier we would need to always provide a response.
void query_worker::query(zmq::socket& dealer)
{
    if (stopped())
        return;

    message request(secure_);
    const auto ec = request.receive(dealer);

    if (ec == error::service_stopped)
        return;

    if (ec)
    {
        LOG_DEBUG(LOG_SERVER)
            << "Failed to receive query from " << request.route().display()
            << " " << ec.message();

        send(message(request, ec), dealer);
        return;
    }

    // Locate the request handler for this command.
    const auto handler = command_handlers_.find(request.command());

    if (handler == command_handlers_.end())
    {
        LOG_DEBUG(LOG_SERVER)
            << "Invalid query command from " << request.route().display();

        send(message(request, error::not_found), dealer);
        return;
    }

    if (verbose_)
        LOG_INFO(LOG_SERVER)
            << "Query " << request.command() << " from "
            << request.route().display();

    // The query executor is the delegate bound by the attach method.
    const auto& query_execute = handler->second;

    // Execute the request and send the result.
    // Example: address.renew(node_, request, sender);
    // Example: blockchain.fetch_history2(node_, request, sender);
    query_execute(request,
        std::bind(&query_worker::send,
            _1, std::ref(dealer)));
}

// Query Interface.
// ----------------------------------------------------------------------------

// Class and method names must match protocol expectations (do not change).
#define ATTACH(class_name, method_name, node) \
    attach(#class_name "." #method_name, \
        std::bind(&bc::server::class_name::method_name, \
            std::ref(node), _1, _2));

void query_worker::attach(const std::string& command,
    command_handler handler)
{
    command_handlers_[command] = handler;
}

//=============================================================================
// TODO: add to client:
// address.unsubscribe2
// blockchain.fetch_spend
// blockchain.fetch_block_height
// blockchain.fetch_block_transaction_hashes
// blockchain.fetch_stealth_transaction
// protocol.total_connections
//=============================================================================
// address.fetch_history is obsoleted in v3 (no unonfirmed tx indexing).
// address.renew is obsoleted in v3.
// address.subscribe is obsoleted in v3.
// address.subscribe2 is new in v3, also call for renew.
// address.unsubscribe2 is new in v3 (there was never an address.unsubscribe).
//-----------------------------------------------------------------------------
// blockchain.validate is new in v3 (blocks).
// blockchain.broadcast is new in v3 (blocks).
// blockchain.fetch_history is obsoleted in v3 (hash reversal).
// blockchain.fetch_history2 is new in v3.
// blockchain.fetch_stealth is obsoleted in v3 (hash reversal).
// blockchain.fetch_stealth2 is new in v3.
// blockchain.fetch_stealth_transaction is new in v3 (safe version).
//-----------------------------------------------------------------------------
// transaction_pool.validate is obsoleted in v3 (sends unconfirmed outputs).
// transaction_pool.validate2 is new in v3.
// transaction_pool.broadcast is new in v3 (rename).
// transaction_pool.fetch_transaction is enhanced in v3 (adds confirmed txs).
//-----------------------------------------------------------------------------
// protocol.broadcast_transaction is obsoleted in v3 (renamed).
//=============================================================================
// Interface class.method names must match protocol (do not change).
void query_worker::attach_interface()
{
    ////ATTACH(address, renew, node_);                          // obsoleted
    ////ATTACH(address, subscribe, node_);                      // obsoleted
    ////ATTACH(address, fetch_history, node_);                  // obsoleted
    ATTACH(address, subscribe2, node_);                         // new
    ATTACH(address, unsubscribe2, node_);                       // new

    ////ATTACH(blockchain, fetch_stealth, node_);               // obsoleted
    ////ATTACH(blockchain, fetch_history, node_);               // obsoleted
    ATTACH(blockchain, fetch_block_header, node_);              // original
    ATTACH(blockchain, fetch_block_height, node_);              // original
    ATTACH(blockchain, fetch_block_transaction_hashes, node_);  // original
    ATTACH(blockchain, fetch_last_height, node_);               // original
    ATTACH(blockchain, fetch_transaction, node_);               // original
    ATTACH(blockchain, fetch_transaction_index, node_);         // original
    ATTACH(blockchain, fetch_spend, node_);                     // original
    ATTACH(blockchain, fetch_history2, node_);                  // new
    ATTACH(blockchain, fetch_stealth2, node_);                  // new
    ATTACH(blockchain, fetch_stealth_transaction, node_);       // new
    ATTACH(blockchain, broadcast, node_);                       // new
    ATTACH(blockchain, validate, node_);                        // new

    ////ATTACH(transaction_pool, validate, node_);              // obsoleted
    ATTACH(transaction_pool, fetch_transaction, node_);         // enhanced
    ATTACH(transaction_pool, broadcast, node_);                 // new
    ATTACH(transaction_pool, validate2, node_);                 // new

    ////ATTACH(protocol, broadcast_transaction, node_);         // obsoleted
    ATTACH(protocol, total_connections, node_);                 // original
}

#undef ATTACH

} // namespace server
} // namespace libbitcoin
