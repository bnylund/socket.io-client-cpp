#ifndef SIO_CLIENT_IMPL_HPP
#define SIO_CLIENT_IMPL_HPP

#include <cstdint>
#ifdef _WIN32
#define _WEBSOCKETPP_CPP11_THREAD_
//#define _WEBSOCKETPP_CPP11_RANDOM_DEVICE_
#define _WEBSOCKETPP_NO_CPP11_FUNCTIONAL_
#define INTIALIZER(__TYPE__)
#else
#define _WEBSOCKETPP_CPP11_STL_ 1
#define INTIALIZER(__TYPE__) (__TYPE__)
#endif
#include <websocketpp/client.hpp>
#if _DEBUG || DEBUG
#if SIO_TLS
#include <websocketpp/config/debug_asio.hpp>
typedef websocketpp::config::debug_asio_tls client_config;
#else
#include <websocketpp/config/debug_asio_no_tls.hpp>
typedef websocketpp::config::debug_asio client_config;
#endif //SIO_TLS
#else
#if SIO_TLS
#include <websocketpp/config/asio_client.hpp>
typedef websocketpp::config::asio_tls_client client_config;
#else
#include <websocketpp/config/asio_no_tls_client.hpp>
typedef websocketpp::config::asio_client client_config;
#endif //SIO_TLS
#endif //DEBUG

#if SIO_TLS
#include <asio/ssl/context.hpp>
#endif

// Comment this out to disable handshake logging to stdout
#if DEBUG || _DEBUG
#define LOG(x) std::cout << x
#else
#define LOG(x)
#endif

#if SIO_TLS
// If using Asio's SSL support, you will also need to add this #include.
// Source: http://think-async.com/Asio/asio-1.10.6/doc/asio/using.html
// #include <asio/ssl/impl/src.hpp>
#endif

using std::chrono::milliseconds;
using namespace std;

#include <asio/steady_timer.hpp>
#include <asio/error_code.hpp>
#include <asio/io_service.hpp>

#include <functional>
#include <sstream>
#include <chrono>
#include <mutex>
#include <cmath>
#include <atomic>
#include <memory>
#include <map>
#include <thread>
#include "../sio_client.hpp"
#include "sio_packet.hpp"

namespace sio
{
    using namespace websocketpp;
    
    typedef websocketpp::client<client_config> client_type;
    
    class client_impl {
        
    protected:
        enum con_state
        {
            con_opening,
            con_opened,
            con_closing,
            con_closed
        };
        
        template <bool tls>
        client_impl():
            m_ping_interval(0),
            m_ping_timeout(0),
            m_network_thread(),
            m_con_state(con_closed),
            m_reconn_delay(5000),
            m_reconn_delay_max(25000),
            m_reconn_attempts(0xFFFFFFFF),
            m_reconn_made(0),
            tls(tls)
        {
            using websocketpp::log::alevel;
#ifndef DEBUG
            m_client.clear_access_channels(alevel::all);
            m_client.set_access_channels(alevel::connect|alevel::disconnect|alevel::app);
#endif
            // Initialize the Asio transport policy
            m_client.init_asio();

            // Bind the clients we are using
            using std::placeholders::_1;
            using std::placeholders::_2;
            m_client.set_open_handler(std::bind(&client_impl::on_open,this,_1));
            m_client.set_close_handler(std::bind(&client_impl::on_close,this,_1));
            m_client.set_fail_handler(std::bind(&client_impl::on_fail,this,_1));
            m_client.set_message_handler(std::bind(&client_impl::on_message,this,_1,_2));
            if(tls)
                m_client.set_tls_init_handler(std::bind(&client_impl::on_tls_init,this,_1));
            m_packet_mgr.set_decode_callback(std::bind(&client_impl::on_decode,this,_1));
            m_packet_mgr.set_encode_callback(std::bind(&client_impl::on_encode,this,_1,_2));
        }
        
        ~client_impl() {
            this->sockets_invoke_void(&sio::socket::on_close);
            sync_close();
        }
        
        //set listeners and event bindings.
#define SYNTHESIS_SETTER(__TYPE__,__FIELD__) \
    void set_##__FIELD__(__TYPE__ const& l) \
        { m_##__FIELD__ = l;}
        
        SYNTHESIS_SETTER(client::con_listener,open_listener)
        
        SYNTHESIS_SETTER(client::con_listener,fail_listener)

        SYNTHESIS_SETTER(client::reconnect_listener,reconnect_listener)

        SYNTHESIS_SETTER(client::con_listener,reconnecting_listener)
        
        SYNTHESIS_SETTER(client::close_listener,close_listener)
        
        SYNTHESIS_SETTER(client::socket_listener,socket_open_listener)
        
        SYNTHESIS_SETTER(client::socket_listener,socket_close_listener)
        
#undef SYNTHESIS_SETTER
        
        
        void clear_con_listeners()
        {
            m_open_listener = nullptr;
            m_close_listener = nullptr;
            m_fail_listener = nullptr;
            m_reconnect_listener = nullptr;
            m_reconnecting_listener = nullptr;
        }
        
        void clear_socket_listeners()
        {
            m_socket_open_listener = nullptr;
            m_socket_close_listener = nullptr;
        }
        
        // Client Functions - such as send, etc.
        void connect(const std::string& uri, const std::map<std::string, std::string>& queryString,
                     const std::map<std::string, std::string>& httpExtraHeaders, const message::ptr& auth){
            if(m_reconn_timer)
            {
                m_reconn_timer->cancel();
                m_reconn_timer.reset();
            }
            if(m_network_thread)
            {
                if(m_con_state == con_closing||m_con_state == con_closed)
                {
                    //if client is closing, join to wait.
                    //if client is closed, still need to join,
                    //but in closed case,join will return immediately.
                    m_network_thread->join();
                    m_network_thread.reset();//defensive
                }
                else
                {
                    //if we are connected, do nothing.
                    return;
                }
            }
            m_con_state = con_opening;
            m_base_url = uri;
            m_reconn_made = 0;

            string query_str;
            for(map<string,string>::const_iterator it=query.begin();it!=query.end();++it){
                query_str.append("&");
                query_str.append(it->first);
                query_str.append("=");
                string query_str_value=encode_query_string(it->second);
                query_str.append(query_str_value);
            }
            m_query_string=move(query_str);

            m_http_headers = headers;
            m_auth = auth;

            this->reset_states();
            m_abort_retries = false;
            m_client.get_io_service().dispatch(std::bind(&client_impl::connect_impl,this,uri,m_query_string));
            m_network_thread.reset(new thread(std::bind(&client_impl::run_loop,this)));//uri lifecycle?

        }
        
        sio::socket::ptr const& socket(const std::string& nsp) {
            lock_guard<mutex> guard(m_socket_mutex);
            string aux;
            if(nsp == "")
            {
                aux = "/";
            }
            else if( nsp[0] != '/')
            {
                aux.append("/",1);
                aux.append(nsp);
            }
            else
            {
                aux = nsp;
            }

            auto it = m_sockets.find(aux);
            if(it!= m_sockets.end())
            {
                return it->second;
            }
            else
            {
                pair<const string, socket::ptr> p(aux,shared_ptr<sio::socket>(new sio::socket(this,aux,m_auth)));
                return (m_sockets.insert(p).first)->second;
            }
        }
        
        // Closes the connection
        void close() {
            m_con_state = con_closing;
            m_abort_retries = true;
            this->sockets_invoke_void(&sio::socket::close);
            m_client.get_io_service().dispatch(std::bind(&client_impl::close_impl, this,close::status::normal,"End by user"));
        }
        
        void sync_close() {
            m_con_state = con_closing;
            m_abort_retries = true;
            this->sockets_invoke_void(&sio::socket::close);
            m_client.get_io_service().dispatch(std::bind(&client_impl::close_impl, this,close::status::normal,"End by user"));
            if(m_network_thread)
            {
                m_network_thread->join();
                m_network_thread.reset();
            }
        }
        
        bool opened() const { return m_con_state == con_opened; }
        
        std::string const& get_sessionid() const { return m_sid; }

        void set_reconnect_attempts(unsigned attempts) {m_reconn_attempts = attempts;}

        void set_reconnect_delay(unsigned millis) {m_reconn_delay = millis;if(m_reconn_delay_max<millis) m_reconn_delay_max = millis;}

        void set_reconnect_delay_max(unsigned millis) {m_reconn_delay_max = millis;if(m_reconn_delay>millis) m_reconn_delay = millis;}

        void set_logs_default() {
            m_client.clear_access_channels(websocketpp::log::alevel::all);
            m_client.set_access_channels(websocketpp::log::alevel::connect | websocketpp::log::alevel::disconnect | websocketpp::log::alevel::app);
        }

        void set_logs_quiet() {
            m_client.clear_access_channels(websocketpp::log::alevel::all);
        }

        void set_logs_verbose() {
            m_client.set_access_channels(websocketpp::log::alevel::all);
        }

    protected:
        void send(packet& p) {
            m_packet_mgr.encode(p);
        }
        
        void remove_socket(std::string const& nsp) {
            lock_guard<mutex> guard(m_socket_mutex);
            auto it = m_sockets.find(nsp);
            if(it!= m_sockets.end())
            {
                m_sockets.erase(it);
            }
        }
        
        asio::io_service& get_io_service() {
            return m_client.get_io_service();
        }
        
        void on_socket_closed(std::string const& nsp) {
            if(m_socket_close_listener) m_socket_close_listener(nsp);
        }
        
        void on_socket_opened(std::string const& nsp) {
            if(m_socket_open_listener) m_socket_open_listener(nsp);
        }
        
    private:
        void run_loop() {
            m_client.run();
            m_client.reset();
            m_client.get_alog().write(websocketpp::log::alevel::devel, "run loop end");
        }

        void connect_impl(const std::string& uri, const std::string& query) {
            do {
                websocketpp::uri uo(uri);
                ostringstream ss;
                if(tls)
                    ss<<"wss://";
                else
                    ss<<"ws://";
                const std::string host(uo.get_host());
                // As per RFC2732, literal IPv6 address should be enclosed in "[" and "]".
                if(host.find(':')!=std::string::npos){
                    ss<<"["<<uo.get_host()<<"]";
                } else {
                    ss<<uo.get_host();
                }

                // If a resource path was included in the URI, use that, otherwise
                // use the default /socket.io/.
                const std::string path(uo.get_resource() == "/" ? "/socket.io/" : uo.get_resource());

                ss<<":"<<uo.get_port()<<path<<"?EIO=4&transport=websocket";
                if(m_sid.size()>0){
                    ss<<"&sid="<<m_sid;
                }
                ss<<"&t="<<time(NULL)<<queryString;
                lib::error_code ec;
                client_type::connection_ptr con = m_client.get_connection(ss.str(), ec);
                if (ec) {
                    m_client.get_alog().write(websocketpp::log::alevel::app,
                                            "Get Connection Error: "+ec.message());
                    break;
                }

                for( auto&& header: m_http_headers ) {
                    con->replace_header(header.first, header.second);
                }

                m_client.connect(con);
                return;
            }
            while(0);
            if(m_fail_listener)
            {
                m_fail_listener();
            }
        }

        void close_impl(close::status::value const& code,std::string const& reason) {
            LOG("Close by reason:"<<reason << endl);
            if(m_reconn_timer)
            {
                m_reconn_timer->cancel();
                m_reconn_timer.reset();
            }
            if (m_con.expired())
            {
                cerr << "Error: No active session" << endl;
            }
            else
            {
                lib::error_code ec;
                m_client.close(m_con, code, reason, ec);
            }
        }
        
        void send_impl(std::shared_ptr<const std::string> const&  payload_ptr,frame::opcode::value opcode) {
            if(m_con_state == con_opened)
            {
                lib::error_code ec;
                m_client.send(m_con,*payload_ptr,opcode,ec);
                if(ec)
                {
                    cerr<<"Send failed,reason:"<< ec.message()<<endl;
                }
            }
        }
        
        void ping(const asio::error_code& ec);
        
        void timeout_ping(const asio::error_code& ec) {
            if(ec)
            {
                return;
            }
            LOG("Ping timeout"<<endl);
            m_client.get_io_service().dispatch(std::bind(&client_impl::close_impl, this,close::status::policy_violation,"Ping timeout"));
        }

        void timeout_reconnect(asio::error_code const& ec) {
            if(ec)
            {
                return;
            }
            if(m_con_state == con_closed)
            {
                m_con_state = con_opening;
                m_reconn_made++;
                this->reset_states();
                LOG("Reconnecting..."<<endl);
                if(m_reconnecting_listener) m_reconnecting_listener();
                m_client.get_io_service().dispatch(std::bind(&client_impl::connect_impl,this,m_base_url,m_query_string));
            }
        }

        unsigned next_delay() const {
            //no jitter, fixed power root.
            unsigned reconn_made = min<unsigned>(m_reconn_made,32);//protect the pow result to be too big.
            return static_cast<unsigned>(min<double>(m_reconn_delay * pow(1.5,reconn_made),m_reconn_delay_max));
        }

        socket::ptr get_socket_locked(std::string const& nsp) {
            lock_guard<mutex> guard(m_socket_mutex);
            auto it = m_sockets.find(nsp);
            if(it != m_sockets.end())
            {
                return it->second;
            }
            else
            {
                return socket::ptr();
            }
        }
        
        void sockets_invoke_void(void (sio::socket::*fn)(void)) {
            map<const string,socket::ptr> socks;
            {
                lock_guard<mutex> guard(m_socket_mutex);
                socks.insert(m_sockets.begin(),m_sockets.end());
            }
            for (auto it = socks.begin(); it!=socks.end(); ++it) {
                ((*(it->second)).*fn)();
            }
        }
        
        void on_decode(packet const& pack) {
            switch(p.get_frame())
            {
            case packet::frame_message:
            {
                socket::ptr so_ptr = get_socket_locked(p.get_nsp());
                if(so_ptr)so_ptr->on_message_packet(p);
                break;
            }
            case packet::frame_open:
                this->on_handshake(p.get_message());
                break;
            case packet::frame_close:
                //FIXME how to deal?
                this->close_impl(close::status::abnormal_close, "End by server");
                break;
            case packet::frame_ping:
                this->on_ping();
                break;

            default:
                break;
            }
        }

        void on_encode(bool isBinary,shared_ptr<const string> const& payload) {
            LOG("encoded payload length:"<<payload->length()<<endl);
            m_client.get_io_service().dispatch(std::bind(&client_impl::send_impl,this,payload,isBinary?frame::opcode::binary:frame::opcode::text));
        }
        
        //websocket callbacks
        void on_fail(connection_hdl con) {
            if (m_con_state == con_closing) {
                LOG("Connection failed while closing." << endl);
                this->close();
                return;
            }

            m_con.reset();
            m_con_state = con_closed;
            this->sockets_invoke_void(&sio::socket::on_disconnect);
            LOG("Connection failed." << endl);
            if(m_reconn_made<m_reconn_attempts && !m_abort_retries)
            {
                LOG("Reconnect for attempt:"<<m_reconn_made<<endl);
                unsigned delay = this->next_delay();
                if(m_reconnect_listener) m_reconnect_listener(m_reconn_made,delay);
                m_reconn_timer.reset(new asio::steady_timer(m_client.get_io_service()));
                asio::error_code ec;
                m_reconn_timer->expires_from_now(milliseconds(delay), ec);
                m_reconn_timer->async_wait(std::bind(&client_impl::timeout_reconnect,this, std::placeholders::_1));
            }
            else
            {
                if(m_fail_listener)m_fail_listener();
            }
        }

        void on_open(connection_hdl con) {
            if (m_con_state == con_closing) {
                LOG("Connection opened while closing." << endl);
                this->close();
                return;
            }

            LOG("Connected." << endl);
            m_con_state = con_opened;
            m_con = con;
            m_reconn_made = 0;
            this->sockets_invoke_void(&sio::socket::on_open);
            this->socket("");
            if(m_open_listener)m_open_listener();
        }

        void on_close(connection_hdl con) {
            LOG("Client Disconnected." << endl);
            con_state m_con_state_was = m_con_state;
            m_con_state = con_closed;
            lib::error_code ec;
            close::status::value code = close::status::normal;
            client_type::connection_ptr conn_ptr  = m_client.get_con_from_hdl(con, ec);
            if (ec) {
                LOG("OnClose get conn failed"<<ec<<endl);
            }
            else
            {
                code = conn_ptr->get_local_close_code();
            }
            
            m_con.reset();
            this->clear_timers();
            client::close_reason reason;

            // If we initiated the close, no matter what the close status was,
            // we'll consider it a normal close. (When using TLS, we can
            // sometimes get a TLS Short Read error when closing.)
            if(code == close::status::normal || m_con_state_was == con_closing)
            {
                this->sockets_invoke_void(&sio::socket::on_disconnect);
                reason = client::close_reason_normal;
            }
            else
            {
                this->sockets_invoke_void(&sio::socket::on_disconnect);
                if(m_reconn_made<m_reconn_attempts && !m_abort_retries)
                {
                    LOG("Reconnect for attempt:"<<m_reconn_made<<endl);
                    unsigned delay = this->next_delay();
                    if(m_reconnect_listener) m_reconnect_listener(m_reconn_made,delay);
                    m_reconn_timer.reset(new asio::steady_timer(m_client.get_io_service()));
                    asio::error_code ec;
                    m_reconn_timer->expires_from_now(milliseconds(delay), ec);
                    m_reconn_timer->async_wait(std::bind(&client_impl::timeout_reconnect,this, std::placeholders::_1));
                    return;
                }
                reason = client::close_reason_drop;
            }
            
            if(m_close_listener)
            {
                m_close_listener(reason);
            }
        }

        void on_message(connection_hdl con, client_type::message_ptr msg) {
            // Parse the incoming message according to socket.IO rules
            m_packet_mgr.put_payload(msg->get_payload());
        }

        //socketio callbacks
        void on_handshake(message::ptr const& message) {
            if(message && message->get_flag() == message::flag_object)
            {
                const object_message* obj_ptr =static_cast<object_message*>(message.get());
                const map<string,message::ptr>* values = &(obj_ptr->get_map());
                auto it = values->find("sid");
                if (it!= values->end()) {
                    m_sid = static_pointer_cast<string_message>(it->second)->get_string();
                }
                else
                {
                    goto failed;
                }
                it = values->find("pingInterval");
                if (it!= values->end()&&it->second->get_flag() == message::flag_integer) {
                    m_ping_interval = (unsigned)static_pointer_cast<int_message>(it->second)->get_int();
                }
                else
                {
                    m_ping_interval = 25000;
                }
                it = values->find("pingTimeout");

                if (it!=values->end()&&it->second->get_flag() == message::flag_integer) {
                    m_ping_timeout = (unsigned) static_pointer_cast<int_message>(it->second)->get_int();
                }
                else
                {
                    m_ping_timeout = 60000;
                }

                // Start ping timeout
                update_ping_timeout_timer();

                return;
            }
    failed:
            //just close it.
            m_client.get_io_service().dispatch(std::bind(&client_impl::close_impl, this,close::status::policy_violation,"Handshake error"));
        }


        void on_ping() {
            // Reply with pong packet.
            packet p(packet::frame_pong);
            m_packet_mgr.encode(p, [&](bool /*isBin*/,shared_ptr<const string> payload)
            {
                this->m_client.send(this->m_con, *payload, frame::opcode::text);
            });

            // Reset the ping timeout.
            update_ping_timeout_timer();
        }

        void reset_states() {
            m_client.reset();
            m_sid.clear();
            m_packet_mgr.reset();
        }

        void clear_timers() {
            LOG("clear timers"<<endl);
            asio::error_code ec;
            if(m_ping_timeout_timer)
            {
                m_ping_timeout_timer->cancel(ec);
                m_ping_timeout_timer.reset();
            }
        }

        void update_ping_timeout_timer() {
            if (!m_ping_timeout_timer) {
                m_ping_timeout_timer = std::unique_ptr<asio::steady_timer>(new asio::steady_timer(get_io_service()));
            }

            asio::error_code ec;
            m_ping_timeout_timer->expires_from_now(milliseconds(m_ping_interval + m_ping_timeout), ec);
            m_ping_timeout_timer->async_wait(std::bind(&client_impl::timeout_ping, this, std::placeholders::_1));
        }
        
        typedef websocketpp::lib::shared_ptr<asio::ssl::context> context_ptr;
        
        context_ptr on_tls_init(connection_hdl con) {
            context_ptr ctx = context_ptr(new  asio::ssl::context(asio::ssl::context::tls));
            asio::error_code ec;
            ctx->set_options(asio::ssl::context::default_workarounds |
                            asio::ssl::context::no_tlsv1 |
                            asio::ssl::context::no_tlsv1_1 |
                            asio::ssl::context::single_dh_use,ec);
            if(ec)
            {
                cerr<<"Init tls failed,reason:"<< ec.message()<<endl;
            }
            
            return ctx;
        }
        
        // Percent encode query string
        std::string encode_query_string(const std::string &query) {
            ostringstream ss;
            ss << std::hex;
            // Percent-encode (RFC3986) non-alphanumeric characters.
            for(const char c : query){
                if((c >= 'a' && c <= 'z') || (c>= 'A' && c<= 'Z') || (c >= '0' && c<= '9')){
                    ss << c;
                } else {
                    ss << '%' << std::uppercase << std::setw(2) << int((unsigned char) c) << std::nouppercase;
                }
            }
            ss << std::dec;
            return ss.str();
        }

        // Connection pointer for client functions.
        connection_hdl m_con;
        client_type m_client;
        // Socket.IO server settings
        std::string m_sid;
        std::string m_base_url;
        std::string m_query_string;
        std::map<std::string, std::string> m_http_headers;
        message::ptr m_auth;

        unsigned int m_ping_interval;
        unsigned int m_ping_timeout;
        
        std::unique_ptr<std::thread> m_network_thread;
        
        packet_manager m_packet_mgr;
        
        std::unique_ptr<asio::steady_timer> m_ping_timeout_timer;

        std::unique_ptr<asio::steady_timer> m_reconn_timer;
        
        con_state m_con_state;
        
        client::con_listener m_open_listener;
        client::con_listener m_fail_listener;
        client::con_listener m_reconnecting_listener;
        client::reconnect_listener m_reconnect_listener;
        client::close_listener m_close_listener;
        
        client::socket_listener m_socket_open_listener;
        client::socket_listener m_socket_close_listener;
        
        std::map<const std::string,socket::ptr> m_sockets;
        
        std::mutex m_socket_mutex;

        unsigned m_reconn_delay;

        unsigned m_reconn_delay_max;

        unsigned m_reconn_attempts;

        unsigned m_reconn_made;

        bool tls;

        std::atomic<bool> m_abort_retries { false };

        friend class sio::client;
        friend class sio::socket;
    };
}
#endif // SIO_CLIENT_IMPL_HPP

