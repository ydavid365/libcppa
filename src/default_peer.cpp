/******************************************************************************\
 *           ___        __                                                    *
 *          /\_ \    __/\ \                                                   *
 *          \//\ \  /\_\ \ \____    ___   _____   _____      __               *
 *            \ \ \ \/\ \ \ '__`\  /'___\/\ '__`\/\ '__`\  /'__`\             *
 *             \_\ \_\ \ \ \ \L\ \/\ \__/\ \ \L\ \ \ \L\ \/\ \L\.\_           *
 *             /\____\\ \_\ \_,__/\ \____\\ \ ,__/\ \ ,__/\ \__/.\_\          *
 *             \/____/ \/_/\/___/  \/____/ \ \ \/  \ \ \/  \/__/\/_/          *
 *                                          \ \_\   \ \_\                     *
 *                                           \/_/    \/_/                     *
 *                                                                            *
 * Copyright (C) 2011-2013                                                    *
 * Dominik Charousset <dominik.charousset@haw-hamburg.de>                     *
 *                                                                            *
 * This file is part of libcppa.                                              *
 * libcppa is free software: you can redistribute it and/or modify it under   *
 * the terms of the GNU Lesser General Public License as published by the     *
 * Free Software Foundation, either version 3 of the License                  *
 * or (at your option) any later version.                                     *
 *                                                                            *
 * libcppa is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                       *
 * See the GNU Lesser General Public License for more details.                *
 *                                                                            *
 * You should have received a copy of the GNU Lesser General Public License   *
 * along with libcppa. If not, see <http://www.gnu.org/licenses/>.            *
\******************************************************************************/


#include <cstring>
#include <cstdint>

#include "cppa/on.hpp"
#include "cppa/actor.hpp"
#include "cppa/match.hpp"
#include "cppa/logging.hpp"
#include "cppa/to_string.hpp"
#include "cppa/exit_reason.hpp"
#include "cppa/actor_proxy.hpp"
#include "cppa/binary_serializer.hpp"
#include "cppa/binary_deserializer.hpp"

#include "cppa/detail/demangle.hpp"
#include "cppa/detail/actor_registry.hpp"
#include "cppa/detail/singleton_manager.hpp"

#include "cppa/network/middleman.hpp"
#include "cppa/network/default_peer.hpp"
#include "cppa/network/message_header.hpp"
#include "cppa/network/default_protocol.hpp"

using namespace std;

namespace cppa { namespace network {

default_peer::default_peer(default_protocol* parent,
                           const input_stream_ptr& in,
                           const output_stream_ptr& out,
                           process_information_ptr peer_ptr)
: super(in->read_handle(), out->write_handle())
, m_parent(parent), m_in(in), m_out(out)
, m_state((peer_ptr) ? wait_for_msg_size : wait_for_process_info)
, m_node(peer_ptr)
, m_has_unwritten_data(false) {
    m_rd_buf.reset(m_state == wait_for_process_info
                   ? sizeof(uint32_t) + process_information::node_id_size
                   : sizeof(uint32_t));
    // state == wait_for_msg_size iff peer was created using remote_peer()
    // in this case, this peer must be erased if no proxy of it remains
    m_erase_on_last_proxy_exited = m_state == wait_for_msg_size;
    m_meta_hdr = uniform_typeid<message_header>();
    m_meta_msg = uniform_typeid<any_tuple>();
}

default_peer::~default_peer() {
    disconnected();
}

void default_peer::disconnected() {
    CPPA_LOG_TRACE("node = " << (m_node ? to_string(*m_node) : "nullptr"));
    if (m_node) {
        // kill all proxies
        auto& children = m_parent->addressing()->proxies(*m_node);
        for (auto& kvp : children) {
            auto ptr = kvp.second.promote();
            if (ptr) ptr->enqueue(nullptr,
                                  make_any_tuple(atom("KILL_PROXY"),
                                      exit_reason::remote_link_unreachable));
        }
        m_parent->addressing()->erase(*m_node);
    }
}

void default_peer::io_failed() {
    CPPA_LOG_TRACE("");
    disconnected();
}

continue_reading_result default_peer::continue_reading() {
    CPPA_LOG_TRACE("");
    for (;;) {
        try { m_rd_buf.append_from(m_in.get()); }
        catch (exception&) {
            disconnected();
            return read_failure;
        }
        if (!m_rd_buf.full()) return read_continue_later; // try again later
        switch (m_state) {
            case wait_for_process_info: {
                //DEBUG("peer_connection::continue_reading: "
                //      "wait_for_process_info");
                uint32_t process_id;
                process_information::node_id_type node_id;
                memcpy(&process_id, m_rd_buf.data(), sizeof(uint32_t));
                memcpy(node_id.data(), m_rd_buf.data() + sizeof(uint32_t),
                       process_information::node_id_size);
                m_node.reset(new process_information(process_id, node_id));
                if (*process_information::get() == *m_node) {
                    std::cerr << "*** middleman warning: "
                                 "incoming connection from self"
                              << std::endl;
                    return read_failure;
                }
                CPPA_LOG_DEBUG("read process info: " << to_string(*m_node));
                m_parent->register_peer(*m_node, this);
                // initialization done
                m_state = wait_for_msg_size;
                m_rd_buf.reset(sizeof(uint32_t));
                break;
            }
            case wait_for_msg_size: {
                //DEBUG("peer_connection::continue_reading: wait_for_msg_size");
                uint32_t msg_size;
                memcpy(&msg_size, m_rd_buf.data(), sizeof(uint32_t));
                m_rd_buf.reset(msg_size);
                m_state = read_message;
                break;
            }
            case read_message: {
                //DEBUG("peer_connection::continue_reading: read_message");
                message_header hdr;
                any_tuple msg;
                binary_deserializer bd(m_rd_buf.data(), m_rd_buf.size(),
                                       m_parent->addressing());
                try {
                    m_meta_hdr->deserialize(&hdr, &bd);
                    m_meta_msg->deserialize(&msg, &bd);
                }
                catch (exception& e) {
                    CPPA_LOG_ERROR("exception during read_message: "
                                   << detail::demangle(typeid(e))
                                   << ", what(): " << e.what());
                    return read_failure;
                }
                CPPA_LOG_DEBUG("deserialized: " << to_string(hdr) << " " << to_string(msg));
                //DEBUG("<-- " << to_string(msg));
                match(msg) (
                    // monitor messages are sent automatically whenever
                    // actor_proxy_cache creates a new proxy
                    // note: aid is the *original* actor id
                    on(atom("MONITOR"), arg_match) >> [&](const process_information_ptr& node, actor_id aid) {
                        monitor(hdr.sender, node, aid);
                    },
                    on(atom("KILL_PROXY"), arg_match) >> [&](const process_information_ptr& node, actor_id aid, std::uint32_t reason) {
                        kill_proxy(hdr.sender, node, aid, reason);
                    },
                    on(atom("LINK"), arg_match) >> [&](const actor_ptr& ptr) {
                        link(hdr.sender, ptr);
                    },
                    on(atom("UNLINK"), arg_match) >> [&](const actor_ptr& ptr) {
                        unlink(hdr.sender, ptr);
                    },
                    others() >> [&] {
                        deliver(hdr, move(msg));
                    }
                );
                m_rd_buf.reset(sizeof(uint32_t));
                m_state = wait_for_msg_size;
                break;
            }
            default: {
                CPPA_CRITICAL("illegal state");
            }
        }
        // try to read more (next iteration)
    }
}

void default_peer::monitor(const actor_ptr&,
                           const process_information_ptr& node,
                           actor_id aid) {
    CPPA_LOG_TRACE(CPPA_MARG(node, get) << ", " << CPPA_ARG(aid));
    if (!node) {
        CPPA_LOG_ERROR("received MONITOR from invalid peer");
        return;
    }
    auto entry = detail::singleton_manager::get_actor_registry()->get_entry(aid);
    auto pself = process_information::get();

    if (*node == *pself) {
        CPPA_LOG_ERROR("received 'MONITOR' from pself");
    }
    else if (entry.first == nullptr) {
        if (entry.second == exit_reason::not_exited) {
            CPPA_LOG_ERROR("received MONITOR for unknown "
                           "actor id: " << aid);
        }
        else {
            CPPA_LOG_DEBUG("received MONITOR for an actor "
                           "that already finished "
                           "execution; reply KILL_PROXY");
            // this actor already finished execution;
            // reply with KILL_PROXY message
            // get corresponding peer
            enqueue(make_any_tuple(atom("KILL_PROXY"), pself, aid, entry.second));
        }
    }
    else {
        CPPA_LOG_DEBUG("attach functor to " << entry.first.get());
        default_protocol_ptr proto = m_parent;
        entry.first->attach_functor([=](uint32_t reason) {
            proto->run_later([=] {
                CPPA_LOGF_TRACE("lambda from default_peer::monitor");
                auto p = proto->get_peer(*node);
                if (p) p->enqueue(make_any_tuple(atom("KILL_PROXY"), pself, aid, reason));
            });
        });
    }
}

void default_peer::kill_proxy(const actor_ptr& sender,
                              const process_information_ptr& node,
                              actor_id aid,
                              std::uint32_t reason) {
    CPPA_LOG_TRACE(CPPA_MARG(sender, get)
                   << ", " << CPPA_MARG(node, get)
                   << ", " << CPPA_ARG(aid)
                   << ", " << CPPA_ARG(reason));
    if (!node) {
        CPPA_LOG_ERROR("node = nullptr");
        return;
    }
    if (sender != nullptr) {
        CPPA_LOG_ERROR("sender != nullptr");
        return;
    }
    auto proxy = m_parent->addressing()->get(*node, aid);
    if (proxy) {
        CPPA_LOG_DEBUG("received KILL_PROXY for " << aid
                       << ":" << to_string(*node));
        proxy->enqueue(nullptr,
                       make_any_tuple(
                           atom("KILL_PROXY"), reason));
    }
    else {
        CPPA_LOG_INFO("received KILL_PROXY message but "
                      "didn't found a matching instance "
                      "in proxy cache");
    }
}

void default_peer::deliver(const message_header& hdr, any_tuple msg) {
    CPPA_LOG_TRACE("");
    if (hdr.sender && hdr.sender->is_proxy()) {
        hdr.sender.downcast<actor_proxy>()->deliver(hdr, std::move(msg));
    }
    else hdr.deliver(std::move(msg));
    /*
    auto receiver = hdr.receiver.get();
    if (receiver) {
        if (hdr.id.valid()) {
            CPPA_LOG_DEBUG("sync message for actor " << receiver->id());
            receiver->sync_enqueue(hdr.sender.get(), hdr.id, move(msg));
        }
        else {
            CPPA_LOG_DEBUG("async message with "
                           << (hdr.sender ? "" : "in")
                           << "valid sender");
            receiver->enqueue(hdr.sender.get(), move(msg));
        }
    }
    else {
        CPPA_LOG_ERROR("received message with invalid receiver");
    }
    */
}

void default_peer::link(const actor_ptr& sender, const actor_ptr& ptr) {
    // this message is sent from default_actor_proxy in link_to and
    // establish_backling to cause the original actor (sender) to establish
    // a link to ptr as well
    CPPA_LOG_TRACE(CPPA_MARG(sender, get)
                   << ", " << CPPA_MARG(ptr, get));
    CPPA_LOG_ERROR_IF(!sender, "received 'LINK' from invalid sender");
    CPPA_LOG_ERROR_IF(!ptr, "received 'LINK' with invalid target");
    if (!sender || !ptr) return;
    CPPA_LOG_ERROR_IF(sender->is_proxy(),
                      "received 'LINK' for a non-local actor");
    if (ptr->is_proxy()) {
        // make sure to not send a needless 'LINK' message back
        ptr.downcast<actor_proxy>()->local_link_to(sender);
    }
    else sender->link_to(ptr);
    if (ptr && sender && sender->is_proxy()) {
        sender.downcast<actor_proxy>()->local_link_to(ptr);
    }
}

void default_peer::unlink(const actor_ptr& sender, const actor_ptr& ptr) {
    CPPA_LOG_TRACE(CPPA_MARG(sender, get)
                   << ", " << CPPA_MARG(ptr, get));
    CPPA_LOG_ERROR_IF(!sender, "received 'UNLINK' from invalid sender");
    CPPA_LOG_ERROR_IF(!ptr, "received 'UNLINK' with invalid target");
    if (!sender || !ptr) return;
    CPPA_LOG_ERROR_IF(sender->is_proxy(),
                      "received 'UNLINK' for a non-local actor");
    if (ptr->is_proxy()) {
        // make sure to not send a needles 'UNLINK' message back
        ptr.downcast<actor_proxy>()->local_unlink_from(sender);
    }
    else sender->unlink_from(ptr);
}

continue_writing_result default_peer::continue_writing() {
    CPPA_LOG_TRACE("");
    CPPA_LOG_DEBUG_IF(!m_has_unwritten_data, "nothing to write (done)");
    while (m_has_unwritten_data) {
        size_t written;
        try { written = m_out->write_some(m_wr_buf.data(), m_wr_buf.size()); }
        catch (exception& e) {
            CPPA_LOG_ERROR(to_verbose_string(e));
            static_cast<void>(e); // keep compiler happy
            disconnected();
            return write_failure;
        }
        if (written != m_wr_buf.size()) {
            CPPA_LOG_DEBUG("tried to write " << m_wr_buf.size() << "bytes, "
                           << "only " << written << " bytes written");
            m_wr_buf.erase_leading(written);
            return write_continue_later;
        }
        else {
            m_wr_buf.reset();
            m_has_unwritten_data = false;
            CPPA_LOG_DEBUG("write done, " << written << "bytes written");
        }
        // try to write next message in queue
        while (!m_has_unwritten_data && !queue().empty()) {
            auto tmp = queue().pop();
            enqueue(tmp.first, tmp.second);
        }
    }
    if (erase_on_last_proxy_exited() && !has_unwritten_data()) {
        if (m_parent->addressing()->count_proxies(*m_node) == 0) {
            m_parent->last_proxy_exited(this);
        }
    }
    return write_done;
}

continuable_io* default_peer::as_io() {
    return this;
}

void default_peer::enqueue(const message_header& hdr, const any_tuple& msg) {
    CPPA_LOG_TRACE("");
    binary_serializer bs(&m_wr_buf, m_parent->addressing());
    uint32_t size = 0;
    auto before = m_wr_buf.size();
    m_wr_buf.write(sizeof(uint32_t), &size, util::grow_if_needed);
    try { bs << hdr << msg; }
    catch (exception& e) {
        CPPA_LOG_ERROR(to_verbose_string(e));
        cerr << "*** exception in default_peer::enqueue; "
             << to_verbose_string(e)
             << endl;
        return;
    }
    CPPA_LOG_DEBUG("serialized: " << to_string(hdr) << " " << to_string(msg));
    size = (m_wr_buf.size() - before) - sizeof(std::uint32_t);
    // update size in buffer
    memcpy(m_wr_buf.data() + before, &size, sizeof(std::uint32_t));
    CPPA_LOG_DEBUG_IF(m_has_unwritten_data, "still registered for writing");
    if (!m_has_unwritten_data) {
        CPPA_LOG_DEBUG("register for writing");
        m_has_unwritten_data = true;
        m_parent->continue_writer(this);
    }
}

} } // namespace cppa::network
