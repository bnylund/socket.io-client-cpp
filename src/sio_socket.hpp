#ifndef SIO_SOCKET_HPP
#define SIO_SOCKET_HPP
#include "sio_message.hpp"
#include "internal/sio_packet.hpp"
#include "internal/sio_client_impl.hpp"
#include <asio/steady_timer.hpp>
#include <asio/error_code.hpp>
#include <queue>
#include <chrono>
#include <cstdarg>
#include <functional>

#if DEBUG || _DEBUG
#define LOG(x) std::cout << x
#else
#define LOG(x)
#endif

#define NULL_GUARD(_x_)  \
    if(_x_ == NULL) return

namespace sio
{
    class event_adapter
    {
    public:
        static void adapt_func(socket::event_listener_aux  const& func, event& event)
        {
            func(event.get_name(),event.get_message(),event.need_ack(),event.get_ack_message_impl());
        }
        
        static inline socket::event_listener do_adapt(socket::event_listener_aux const& func)
        {
            return std::bind(&event_adapter::adapt_func, func,std::placeholders::_1);
        }
        
        static inline event create_event(std::string const& nsp,std::string const& name,message::list&& message,bool need_ack)
        {
            return event(nsp,name,message,need_ack);
        }
    };
    
    class event
    {
    public:
        const std::string& get_nsp() const {
            return m_nsp;
        }
        
        const std::string& get_name() const {
            return m_name;
        }
        
        const message::ptr& get_message() const {
            if(m_messages.size()>0)
                return m_messages[0];
            else
            {
                static message::ptr null_ptr;
                return null_ptr;
            }
        }

        const message::list& get_messages() const {
            return m_messages;
        }
        
        bool need_ack() const {
            return m_need_ack;
        }
        
        void put_ack_message(message::list const& ack_message) {
            if(m_need_ack)
                m_ack_message = std::move(ack_message);
        }
        
        message::list const& get_ack_message() const {
            return m_ack_message;
        }
        
    protected:
        inline event(std::string const& nsp,std::string const& name,message::list const& messages,bool need_ack) :
            m_nsp(nsp),
            m_name(name),
            m_messages(messages),
            m_need_ack(need_ack) {}

        inline event(std::string const& nsp,std::string const& name,message::list&& messages,bool need_ack) :
            m_nsp(nsp),
            m_name(name),
            m_messages(std::move(messages)),
            m_need_ack(need_ack) {}

        inline message::list& get_ack_message_impl() {
            return m_ack_message;
        }
        
    private:
        const std::string m_nsp;
        const std::string m_name;
        const message::list m_messages;
        const bool m_need_ack;
        message::list m_ack_message;
        
        friend class event_adapter;
    };
    
    class client_impl;
    class packet;
    
    //The name 'socket' is taken from concept of official socket.io.
    class socket
    {
    public:
        typedef std::function<void(const std::string& name,message::ptr const& message,bool need_ack, message::list& ack_message)> event_listener_aux;
        
        typedef std::function<void(event& event)> event_listener;
        
        typedef std::function<void(message::ptr const& message)> error_listener;
        
        typedef std::shared_ptr<socket> ptr;
        
        ~socket() {
            delete m_impl;
        }
        
        void on(std::string const& event_name,event_listener const& func) {
            m_impl->on(event_name, func);
        }
        
        void on(std::string const& event_name,event_listener_aux const& func) {
            m_impl->on(event_name, func);
        }
        
        void off(std::string const& event_name) {
            m_impl->off(event_name);
        }
        
        void off_all() {
            m_impl->off_all();
        }
        
        void close() {
            m_impl->close();
        }
        
        void on_error(error_listener const& l) {
            m_impl->on_error(l);
        }
        
        void off_error() {
            m_impl->off_error();
        }

        void emit(std::string const& name, message::list const& msglist = nullptr, std::function<void (message::list const&)> const& ack = nullptr) {
            m_impl->emit(name, msglist,ack);
        }
        
        std::string const& get_namespace() const {
            return m_impl->get_namespace();
        }
        
    protected:
        socket(client_impl*,std::string const&,message::ptr const&) :
            m_impl(new impl(client,nsp,auth)) {}

        void on_connected() {
            m_impl->on_connected();
        }
        
        void on_close() {
            m_impl->on_close();
        }
        
        void on_open() {
            m_impl->on_open();
        }
        
        void on_disconnect() {
            m_impl->on_disconnect();
        }
        
        void on_message_packet(packet const& p) {
            m_impl->on_message_packet(p);
        }
        
        friend class client_impl;
        
    private:
        //disable copy constructor and assign operator.
        socket(socket const&){}
        void operator=(socket const&){}

        class impl;
        impl *m_impl;
    };

    class socket::impl
    {
    public:
        
        impl(client_impl *, std::string const&, message::ptr const&) :
            m_client(client),
            m_connected(false),
            m_nsp(nsp),
            m_auth(auth)
        {
            NULL_GUARD(client);
            if(m_client->opened())
            {
                send_connect();
            }
        }

        ~impl() {}
        
        unsigned int socket::impl::s_global_event_id = 1;
        
        void on(std::string const& event_name,event_listener_aux const& func) {
            this->on(event_name,event_adapter::do_adapt(func));
        }
        
        void on(std::string const& event_name,event_listener const& func) {
            std::lock_guard<std::mutex> guard(m_event_mutex);
            m_event_binding[event_name] = func;
        }
        
        void off(std::string const& event_name) {
            std::lock_guard<std::mutex> guard(m_event_mutex);
            auto it = m_event_binding.find(event_name);
            if(it!=m_event_binding.end())
            {
                m_event_binding.erase(it);
            }
        }
        
        void off_all() {
            std::lock_guard<std::mutex> guard(m_event_mutex);
            m_event_binding.clear();
        }
        
#define SYNTHESIS_SETTER(__TYPE__,__FIELD__) \
    void set_##__FIELD__(__TYPE__ const& l) \
        { m_##__FIELD__ = l;}
        
        SYNTHESIS_SETTER(error_listener, error_listener) //socket io errors
        
#undef SYNTHESIS_SETTER
        
        void on_error(error_listener const& l) {
            m_error_listener = l;
        }
        
        void off_error() {
            m_error_listener = nullptr;
        }
        
        void close() {
            NULL_GUARD(m_client);
            if(m_connected)
            {
                packet p(packet::type_disconnect,m_nsp);
                send_packet(p);
                
                if(!m_connection_timer)
                {
                    m_connection_timer.reset(new asio::steady_timer(m_client->get_io_service()));
                }
                asio::error_code ec;
                m_connection_timer->expires_from_now(std::chrono::milliseconds(3000), ec);
                m_connection_timer->async_wait(std::bind(&socket::impl::on_close, this));
            }
        }
        
        void emit(std::string const& name, message::list const& msglist, std::function<void (message::list const&)> const& ack) {
            NULL_GUARD(m_client);
            message::ptr msg_ptr = msglist.to_array_message(name);
            int pack_id;
            if(ack)
            {
                pack_id = s_global_event_id++;
                std::lock_guard<std::mutex> guard(m_event_mutex);
                m_acks[pack_id] = ack;
            }
            else
            {
                pack_id = -1;
            }
            packet p(m_nsp, msg_ptr,pack_id);
            send_packet(p);
        }
        
        std::string const& get_namespace() const {return m_nsp;}
        
    protected:
        void on_connected() {
            if(m_connection_timer)
            {
                m_connection_timer->cancel();
                m_connection_timer.reset();
            }
            if(!m_connected)
            {
                m_connected = true;
                m_client->on_socket_opened(m_nsp);

                while (true) {
                    m_packet_mutex.lock();
                    if(m_packet_queue.empty())
                    {
                        m_packet_mutex.unlock();
                        return;
                    }
                    sio::packet front_pack = std::move(m_packet_queue.front());
                    m_packet_queue.pop();
                    m_packet_mutex.unlock();
                    m_client->send(front_pack);
                }
            }
        }
        
        void on_close() {
            NULL_GUARD(m_client);
            sio::client_impl *client = m_client;
            m_client = NULL;

            if(m_connection_timer)
            {
                m_connection_timer->cancel();
                m_connection_timer.reset();
            }
            m_connected = false;
            {
                std::lock_guard<std::mutex> guard(m_packet_mutex);
                while (!m_packet_queue.empty()) {
                    m_packet_queue.pop();
                }
            }
            client->on_socket_closed(m_nsp);
            client->remove_socket(m_nsp);
        }
        
        void on_open() {
            send_connect();
        }
        
        void on_message_packet(packet const& packet) {
            NULL_GUARD(m_client);
            if(p.get_nsp() == m_nsp)
            {
                switch (p.get_type())
                {
                // Connect open
                case packet::type_connect:
                {
                    LOG("Received Message type (Connect)"<<std::endl);

                    this->on_connected();
                    break;
                }
                case packet::type_disconnect:
                {
                    LOG("Received Message type (Disconnect)"<<std::endl);
                    this->on_close();
                    break;
                }
                case packet::type_event:
                case packet::type_binary_event:
                {
                    LOG("Received Message type (Event)"<<std::endl);
                    const message::ptr ptr = p.get_message();
                    if(ptr->get_flag() == message::flag_array)
                    {
                        const array_message* array_ptr = static_cast<const array_message*>(ptr.get());
                        if(array_ptr->get_vector().size() >= 1&&array_ptr->get_vector()[0]->get_flag() == message::flag_string)
                        {
                            const string_message* name_ptr = static_cast<const string_message*>(array_ptr->get_vector()[0].get());
                            message::list mlist;
                            for(size_t i = 1;i<array_ptr->get_vector().size();++i)
                            {
                                mlist.push(array_ptr->get_vector()[i]);
                            }
                            this->on_socketio_event(p.get_nsp(), p.get_pack_id(),name_ptr->get_string(), std::move(mlist));
                        }
                    }

                    break;
                }
                    // Ack
                case packet::type_ack:
                case packet::type_binary_ack:
                {
                    LOG("Received Message type (ACK)"<<std::endl);
                    const message::ptr ptr = p.get_message();
                    if(ptr->get_flag() == message::flag_array)
                    {
                        message::list msglist(ptr->get_vector());
                        this->on_socketio_ack(p.get_pack_id(),msglist);
                    }
                    else
                    {
                        this->on_socketio_ack(p.get_pack_id(),message::list(ptr));
                    }
                    break;
                }
                    // Error
                case packet::type_error:
                {
                    LOG("Received Message type (ERROR)"<<std::endl);
                    this->on_socketio_error(p.get_message());
                    break;
                }
                default:
                    break;
                }
            }
        }
        
        void on_disconnect() {
            NULL_GUARD(m_client);
            if(m_connected)
            {
                m_connected = false;
                std::lock_guard<std::mutex> guard(m_packet_mutex);
                while (!m_packet_queue.empty()) {
                    m_packet_queue.pop();
                }
            }
        }
        
    private:
        
        // Message Parsing callbacks.
        void on_socketio_event(const std::string& nsp, int msgId,const std::string& name, message::list&& message) {
            bool needAck = msgId >= 0;
            event ev = event_adapter::create_event(nsp,name, std::move(message),needAck);
            event_listener func = this->get_bind_listener_locked(name);
            if(func)func(ev);
            if(needAck)
            {
                this->ack(msgId, name, ev.get_ack_message());
            }
        }

        void on_socketio_ack(int msgId, message::list const& message) {
            std::function<void (message::list const&)> l;
            {
                std::lock_guard<std::mutex> guard(m_event_mutex);
                auto it = m_acks.find(msgId);
                if(it!=m_acks.end())
                {
                    l = it->second;
                    m_acks.erase(it);
                }
            }
            if(l)l(message);
        }

        void on_socketio_error(message::ptr const& err_message) {
            if(m_error_listener)m_error_listener(err_message);
        }
        
        event_listener get_bind_listener_locked(string const& event) {
            std::lock_guard<std::mutex> guard(m_event_mutex);
            auto it = m_event_binding.find(event);
            if(it!=m_event_binding.end())
            {
                return it->second;
            }
            return socket::event_listener();
        }
        
        void ack(int msgId,string const& name,message::list const& ack_message) {
            packet p(m_nsp, ack_message.to_array_message(),msgId,true);
            send_packet(p);
        }
        
        void timeout_connection(const asio::error_code &ec) {
            NULL_GUARD(m_client);
            if(ec)
            {
                return;
            }
            m_connection_timer.reset();
            LOG("Connection timeout,close socket."<<std::endl);
            //Should close socket if no connected message arrive.Otherwise we'll never ask for open again.
            this->on_close();
        }
        
        void send_connect() {
            NULL_GUARD(m_client);
            packet p(packet::type_connect, m_nsp, m_auth);
            m_client->send(p);
            m_connection_timer.reset(new asio::steady_timer(m_client->get_io_service()));
            asio::error_code ec;
            m_connection_timer->expires_from_now(std::chrono::milliseconds(20000), ec);
            m_connection_timer->async_wait(std::bind(&socket::impl::timeout_connection,this, std::placeholders::_1));
        }
        
        void send_packet(packet& p) {
            NULL_GUARD(m_client);
            if(m_connected)
            {
                while (true) {
                    m_packet_mutex.lock();
                    if(m_packet_queue.empty())
                    {
                        m_packet_mutex.unlock();
                        break;
                    }
                    sio::packet front_pack = std::move(m_packet_queue.front());
                    m_packet_queue.pop();
                    m_packet_mutex.unlock();
                    m_client->send(front_pack);
                }
                m_client->send(p);
            }
            else
            {
                std::lock_guard<std::mutex> guard(m_packet_mutex);
                m_packet_queue.push(p);
            }
        }
        
        static event_listener s_null_event_listener;
        
        static unsigned int s_global_event_id;
        
        sio::client_impl *m_client;
        
        bool m_connected;
        std::string m_nsp;
        message::ptr m_auth;
        
        std::map<unsigned int, std::function<void (message::list const&)> > m_acks;
        
        std::map<std::string, event_listener> m_event_binding;
        
        error_listener m_error_listener;
        
        std::unique_ptr<asio::steady_timer> m_connection_timer;
        
        std::queue<packet> m_packet_queue;
        
        std::mutex m_event_mutex;

		std::mutex m_packet_mutex;
        
        friend class socket;
    };
}
#endif // SIO_SOCKET_HPP
