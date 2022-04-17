//
//  sio_client.hpp
//
//  Created by Melo Yao on 3/25/15.
//

#ifndef SIO_CLIENT_HPP
#define SIO_CLIENT_HPP
#include <string>
#include <functional>
#include "sio_message.hpp"
#include "sio_socket.hpp"
#include "internal/sio_client_impl.hpp"

using namespace websocketpp;
using std::stringstream;

namespace sio
{
    template <bool tls = false>
    class client {
    public:
        enum close_reason
        {
            close_reason_normal,
            close_reason_drop
        };
        
        typedef std::function<void(void)> con_listener;
        
        typedef std::function<void(close_reason const& reason)> close_listener;

        typedef std::function<void(unsigned, unsigned)> reconnect_listener;
        
        typedef std::function<void(std::string const& nsp)> socket_listener;
        
        client() : m_impl(new client_impl<tls>()) {}
        ~client() {
            delete m_impl;
        }
        
        //set listeners and event bindings.
        void set_open_listener(con_listener const& l) {
            m_impl->set_open_listener(l);
        }
        
        void set_fail_listener(con_listener const& l) {
            m_impl->set_fail_listener(l);
        }
        
        void set_reconnecting_listener(con_listener const& l) {
            m_impl->set_reconnecting_listener(l);
        }

        void set_reconnect_listener(reconnect_listener const& l) {
            m_impl->set_reconnect_listener(l);
        }

        void set_close_listener(close_listener const& l) {
            m_impl->set_close_listener(l);
        }
        
        void set_socket_open_listener(socket_listener const& l) {
            m_impl->set_socket_open_listener(l);
        }
        
        void set_socket_close_listener(socket_listener const& l) {
            m_impl->set_socket_close_listener(l);
        }
        
        void clear_con_listeners() {
            m_impl->clear_con_listeners();
        }
        
        void clear_socket_listeners() {
            m_impl->clear_socket_listeners();
        }
        
        // Client Functions - such as send, etc.
        void connect(const std::string& uri) {
            m_impl->connect(uri, {}, {}, {});
        }

        void connect(const std::string& uri, const message::ptr& auth) {
            m_impl->connect(uri, {}, {}, auth);
        }

        void connect(const std::string& uri, const std::map<std::string,std::string>& query) {
            m_impl->connect(uri, query, {}, {});
        }

        void connect(const std::string& uri, const std::map<std::string,std::string>& query, const message::ptr& auth) {
            m_impl->connect(uri, query, {}, auth);
        }

        void connect(const std::string& uri, const std::map<std::string,std::string>& query,
                     const std::map<std::string,std::string>& http_extra_headers) {
            m_impl->connect(uri, query, http_extra_headers, {});
        }

        void connect(const std::string& uri, const std::map<std::string,std::string>& query,
                     const std::map<std::string,std::string>& http_extra_headers, const message::ptr& auth) {
            m_impl->connect(uri, query, http_extra_headers, auth);
        }

        void set_reconnect_attempts(int attempts) {
            m_impl->set_reconnect_attempts(attempts);
        }   

        void set_reconnect_delay(unsigned millis) {
            m_impl->set_reconnect_delay(millis);
        }

        void set_reconnect_delay_max(unsigned millis) {
            m_impl->set_reconnect_delay_max(millis);
        }

        void set_logs_default() {
            m_impl->set_logs_default();
        }

        void set_logs_quiet() {
            m_impl->set_logs_quiet();
        }

        void set_logs_verbose() {
            m_impl->set_logs_verbose();
        }

        sio::socket::ptr const& socket(const std::string& nsp = "") {
            return m_impl->socket(nsp);
        }
        
        // Closes the connection
        void close() {
            m_impl->close();
        }
        
        void sync_close() {
            m_impl->sync_close();
        }
        
        bool opened() const {
            return m_impl->opened();
        }
        
        std::string const& get_sessionid() const {
            return m_impl->get_sessionid();
        }
        
    private:
        //disable copy constructor and assign operator.
        client(client const&){}
        void operator=(client const&){}
        
        client_impl* m_impl;
    };
    
}


#endif // __SIO_CLIENT__H__
