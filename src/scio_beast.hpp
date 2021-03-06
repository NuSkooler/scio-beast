/*
    Copyright (c) 2017, Bryan D. Ashby 
    All rights reserved. 

    Redistribution and use in source and binary forms, with or without 
    modification, are permitted provided that the following conditions are met: 

      * Redistributions of source code must retain the above copyright notice, 
        this list of conditions and the following disclaimer. 
      * Redistributions in binary form must reproduce the above copyright 
        notice, this list of conditions and the following disclaimer in the 
        documentation and/or other materials provided with the distribution. 

    THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY 
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY 
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER 
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
    OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH 
    DAMAGE. 
*/
#ifndef SOCKETCLUSTER_IO_BEAST_H
#define SOCKETCLUSTER_IO_BEAST_H

#pragma once

//  STL
#include <memory>
#include <queue>
#include <random>

//  Boost
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/thread.hpp>
#include <boost/unordered_map.hpp>
#include <boost/signals2.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>


//  json
#include <json.hpp>

using tcp           = boost::asio::ip::tcp;     //  <boost/asio/ip/tcp.hpp>
using json          = nlohmann::json;           //  json.hpp
namespace websocket = boost::beast::websocket;  //  <boost/beast/websocket.hpp>
namespace ssl       = boost::asio::ssl;         //  <boost/asio/ssl.hpp>

namespace scio_beast {

enum errors {
    protocol_error   = 0,
    unexpected_rid,
    json_parse_failure,
    response_error,
    ack_timeout,
};

namespace detail {
    class scio_error_category
        : public boost::system::error_category
    {
    public:
        const char* name() const BOOST_SYSTEM_NOEXCEPT { return "scio_beast::category"; }
        std::string message(int e) const {
            switch(e) {
                case protocol_error     : return "protocol error";
                case unexpected_rid     : return "unexpected response id (rid)";
                case json_parse_failure : return "json parse failure";
                case response_error     : return "response contains error";
                case ack_timeout        : return "acknowledgement timeout";
                default                 : return "scio_beast::category error";
            }
        }
    };
}   //  end detail ns

inline const boost::system::error_category& get_scio_error_category() {
    static detail::scio_error_category instance;
    return instance;
}

inline boost::system::error_code make_error_code(errors e) {
    return boost::system::error_code(static_cast<int>(e), get_scio_error_category());
}

}   //  end scio_beast ns

namespace boost { namespace system {

    template<>
    struct is_error_code_enum<scio_beast::errors> {
        BOOST_STATIC_CONSTANT(bool, value = true);
    };

}}  //  end boost::system ns

namespace scio_beast {

    namespace detail {
        static const std::string EMPTY_STRING;
    }   //  end detail ns
    
typedef uint64_t CallId;

enum class ChannelState {
    UNSUBSCRIBED,
    PENDING,
    SUBSCRIBED,
};

struct ChannelStateData {
    std::string     name;
    ChannelState    oldState;
    ChannelState    newState;
};

typedef boost::signals2::signal<
    void(const boost::beast::multi_buffer&)
>                                                                       EventHandlerRaw;

typedef boost::signals2::signal<void(const boost::system::error_code&)> EventHandlerError;
typedef boost::signals2::signal<void()>                                 EventHandlerConnecting;
typedef boost::signals2::signal<void(const json&)>                      EventHandlerConnect;
typedef boost::signals2::signal<void(const boost::system::error_code&)> EventConnectAbort;
typedef boost::signals2::signal<void(const boost::system::error_code&)> EventDisconnect;
typedef boost::signals2::signal<void(const std::string&)>               EventHandlerAuthenticate;
typedef boost::signals2::signal<void(const std::string&)>               EventHandlerAuthTokenChange;
typedef boost::signals2::signal<void()>                                 EventHandlerDeauthenticate;
typedef boost::signals2::signal<void(const std::string&)>               EventHandlerSubscribe;

typedef boost::signals2::signal<
    void(const std::string&, const boost::system::error_code&)
>                                                                       EventHandlerSubscribeFail;

typedef boost::signals2::signal<void(const ChannelStateData&)>          EventHandlerSubscriptionStateChange;
typedef boost::signals2::signal<void(const std::string&)>               EventHandlerUnsubscribe;
typedef boost::signals2::signal<void(const json&)>                      EventHandlerChannel;

typedef std::function<void(const json& resp)> EmitEventResponseHandler;

typedef boost::signals2::signal<
    void(
        const std::string& eventName, 
        const json&, EmitEventResponseHandler
    )
>                                                                       EventHandlerEmit;

class ICodecEngine {
public:
    virtual ~ICodecEngine() {}

    virtual std::string encode(const json& obj) = 0;
    virtual json decode(const std::string& payload) = 0;
    virtual bool isBinary() const = 0;
};

//  Port from sc-codec-min-bin @ https://github.com/SocketCluster/sc-codec-min-bin
class CodecEngineMinBin
    : public ICodecEngine
{
public:
    virtual std::string encode(const json& obj) override {
        json o;

        if(obj.is_array()) {
            const size_t sz = obj.size();
            for(size_t i = 0; i < sz; ++i) {
                o[i] = compressSinglePacket(obj[i]);
            }
        } else if(detail::EMPTY_STRING != obj.value("event", "") || 0 != obj.value("rid", 0)) {
            o = compressSinglePacket(obj);
        }

        const std::vector<std::uint8_t> msgpack = json::to_msgpack(o);
        return std::string(msgpack.begin(), msgpack.end());
    }

    virtual json decode(const std::string& payload) override {
        json obj = json::from_msgpack(std::vector<uint8_t>(payload.begin(), payload.end()));

        if(obj.is_array()) {
            const size_t sz = obj.size();
            for(size_t i = 0; i < sz; ++i) {
                decompressSinglePacket(obj[i]);
            }
        } else if(obj.is_object()) {
            decompressSinglePacket(obj);
        }

        return obj;
    }

    virtual bool isBinary() const override { return true; }
private:
    json compressSinglePacket(const json& obj) {
        json compressedObj = obj;

        compressPublishPacket(compressedObj);
        compressEmitPacket(compressedObj);
        compressResponsePacket(compressedObj);

        return compressedObj;
    }

    void decompressSinglePacket(json& obj) {
        decompressEmitPacket(obj);
        decompressPublishPacket(obj);
        decompressResponsePacket(obj);
    }

    void compressPublishPacket(json& obj) {
        try {
            std::string const& eventName    = obj.at("event");
            json const& data                = obj.at("data");

            if("#publish" != eventName) {
                return;
            }

            json a = { eventName, data.at("data") };

            const CallId cid = obj.value("cid", 0);
            if(0 != cid) {
                a.push_back(cid);
            }

            obj["p"] = a;

            eraseMembers(obj, { "event", "data", "cid" } );
        } catch(std::out_of_range) {
            //  nop
        }
    }

    void decompressPublishPacket(json& obj) {
        try {
            json const& p = obj.at("p");

            obj["event"]    = "#publish";
            obj["data"]     = {
                { "channel",    p[0] },
                { "data",       p[1] }
            };

            if(p.size() > 2) {
                obj["cid"] = p[2];
            }

            obj.erase("p");
        } catch(std::out_of_range) {
            //  nop
        }
    }

    void compressEmitPacket(json& obj) {
        try {
            std::string const& eventName = obj.at("event");

            json a = { eventName, obj.at("data") };

            const CallId cid = obj.value("cid", 0);
            if(0 != cid) {
                a.push_back(cid);
            }

            obj["e"] = a;

            eraseMembers(obj, { "event", "data", "cid" } );
        } catch(std::out_of_range) {
            //  nop
        }
    }

    void decompressEmitPacket(json& obj) {
        try {
            json const&e = obj.at("e");

            obj["event"]    = e[0];
            obj["data"]     = e[1];

            if(e.size() > 2) {
                obj["cid"] = e[2];
            }

            obj.erase("e");
        } catch(std::out_of_range) {
            //  nop
        }
    }

    void compressResponsePacket(json& obj) {
        try {
            const CallId rid = obj.at("rid");

            //  :TODO: do error/data need to be optionals with default to null?
            obj["r"] = { rid, obj.at("error"), obj.at("data") };

            eraseMembers(obj, { "rid", "error", "data" } );
        } catch(std::out_of_range) {
            //  nop
        }
    }

    void decompressResponsePacket(json& obj) {
        try {
            json const& r = obj.at("r");

            obj["rid"]      = r[0];

            if(!r[1].is_null()) {
                obj["error"] = r[1];
            }

            if(!r[2].is_null()) {
                obj["data"]     = r[2];
            }

            obj.erase("r");
        } catch(std::out_of_range) {
            //  nop
        }
    }

    void eraseMembers(json& obj, const std::vector<std::string>& names) {
        for(auto const& n : names) {
            obj.erase(n);
        }
    }
};

class ChannelSubscriptionOptions {
public:
    ChannelSubscriptionOptions()
        : waitForAuth(false)
    {       
    }

    bool        waitForAuth;
    json        data;
};

class SCSocket; //  forward

class SCChannel {
public:
    enum EventHandlerIds {
        //  :TODO: drop out
        SubscribeEvent,
        SubscribeFailEvent,
        SubscriptionStateChangeEvent,
        UnsubscribeEvent,
        ChannelEvent,
    };

    SCChannel(const std::string& name, std::shared_ptr<SCSocket> socket)
        : m_name(name)
        , m_socket(socket)
        , m_state(ChannelState::UNSUBSCRIBED)
    {
    }

    std::string const& getName() const { return m_name; }

    boost::signals2::connection watch(const EventHandlerChannel::slot_type& slot) {
        return on<ChannelEvent>(slot);
    }

    void unwatch() {
        std::get<ChannelEvent>(m_eventTable).disconnect_all_slots();
    }

    void unwatch(const boost::signals2::connection& conn) {
        conn.disconnect();
    }   

    template <size_t HandlerId, typename ...Args>
    boost::signals2::connection on(Args&& ...args) {
        return std::get<HandlerId>(m_eventTable).connect(std::forward<Args>(args)...);
    }

    inline void unsubscribe();
    inline void destroy();

    ChannelState getState() const { return m_state; }

private:
    friend class SCSocket;

    typedef std::tuple<
        //  :TODO: dropOut
        EventHandlerSubscribe,
        EventHandlerSubscribeFail,
        EventHandlerSubscriptionStateChange,
        EventHandlerUnsubscribe,
        EventHandlerChannel
    > EventTable;
    
    std::string                     m_name;
    std::shared_ptr<SCSocket>       m_socket;
    EventTable                      m_eventTable;
    ChannelState                    m_state;    

    template<size_t HandlerId, typename ...Args>
    void triggerEvent(Args&& ...args) {
        (std::get<HandlerId>(m_eventTable))(std::forward<Args>(args)...);
    }
};

typedef std::shared_ptr<SCChannel> SCChannelPtr;

class AutoReconnectOptions {
public:
    AutoReconnectOptions()
        : initialDelay(10000)
        , randomness(10000)
        , multiplier(1.5)
        , maxDelay(60000)
    {       
    }

    uint32_t        initialDelay;   //  milliseconds
    uint32_t        randomness;     //  milliseconds
    double          multiplier;     //  default is 1.5
    uint32_t        maxDelay;       //  milliseconds
};

class SecureConnectOptions {
public:
    std::shared_ptr<ssl::context>   context;
};

class ConnectOptions {
public:
    ConnectOptions()
        : host("localhost")
        , port("http")
        , secure(false)
        , path("/socketcluster/")
        , autoReconnect(true)
        , ackTimeout(10)
        , codecEngine(nullptr)
    {       
    }

    ConnectOptions& setHost(const std::string& h) {
        host = h;
        return *this;
    }

    ConnectOptions& setPort(const std::string& p) {
        port = p;
        return *this;
    }

    ConnectOptions& setSecure(const bool enableSecure = true) {
        secure = enableSecure;
        return *this;
    }

    ConnectOptions& setPath(const std::string& p) {
        path = p;
        return *this;
    }

    ConnectOptions& setAutoReconnect(const bool r) {
        autoReconnect = r;
        return *this;
    }

    ConnectOptions& setAckTimeout(const uint32_t t) {
        ackTimeout = t;
        return *this;
    }

    ConnectOptions& setPerMessageDeflate(const bool enabled) {
        perMessageDeflateOpts.client_enable = enabled;
        return *this;
    }

    ConnectOptions& setPerMessageDeflate(const websocket::permessage_deflate& pmd) {
        perMessageDeflateOpts = pmd;
        return *this;
    }

    ConnectOptions& setCodecEngine(std::shared_ptr<ICodecEngine> ce) {
        codecEngine = ce;
        return *this;
    }

    ConnectOptions& setUserAgent(const std::string& ua) {
        userAgent = ua;
        return *this;
    }

    std::string                     host;
    std::string                     port;
    std::string                     userAgent;
    bool                            secure; //  SSL/TLS?
    std::string                     path;
    bool                            autoReconnect;
    AutoReconnectOptions            autoReconnectOptions;
    uint32_t                        ackTimeout;
    SecureConnectOptions            secureOptions;
    std::shared_ptr<ICodecEngine>   codecEngine;
    websocket::permessage_deflate   perMessageDeflateOpts;
};

class SCSocket
    : public std::enable_shared_from_this<SCSocket>
{
public:
    enum EventHandlerIds {
        RawEvent,

        ErrorEvent,

        ConnectingEvent,
        ConnectEvent,
        ConnectAbortEvent,
        DisconnectEvent,

        AuthenticateEvent,
        AuthTokenChangeEvent,
        DeauthenticateEvent,

        SubscribeEvent,
        SubscribeFailEvent,
        SubscriptionStateChangeEvent,
        UnsubscribeEvent,

        EmitEvent
    };

    typedef std::function<void(boost::system::error_code ec, const json& resp)> ResponseHandler;

    enum class State {
        CLOSED,
        CONNECTING,
        OPEN
    };

    enum class AuthState {
        UNAUTHENTICATED,
        AUTHENTICATED,
    };

    typedef std::map<std::string, SCChannelPtr> ChannelSubscriptions;

    explicit SCSocket(const ConnectOptions& connectOptions)
        : m_state(State::CLOSED)
        , m_connectOptions(connectOptions)
        , m_resolver(m_ios)
        , m_sslContext(connectOptions.secureOptions.context)
        , m_nextCallId(1)
        , m_connectAttempts(0)
        , m_pingTimeout(connectOptions.ackTimeout * 1000)   //  seconds -> ms
        , m_pingTimeoutTimer(m_ios)
    {
        if(connectOptions.secure && connectOptions.secureOptions.context) {
            m_wss.reset(new SecureWebSocket(m_ios, *m_sslContext.get()));

            setPerMessageDeflate(m_ws);

            m_wss->binary(haveBinaryCodec());
        } else {
            m_ws.reset(new WebSocket(m_ios));

            setPerMessageDeflate(m_ws);

            m_connectOptions.secure = false;    //  we have no SSL context

            m_ws->binary(haveBinaryCodec());
        }
    }

    void connect() {
        if(State::CLOSED != m_state) {
            return;
        }

        startConnect();

        m_iosThread = boost::thread(std::bind(&SCSocket::ioThread, shared_from_this()));
    }

    boost::system::error_code close() {
        m_state = State::CLOSED;
        //  :TODO: we shoudl be using teardown? http://vinniefalco.github.io/beast/beast/ref/beast__websocket__async_teardown/overload2.html
        boost::system::error_code ec;

        if(m_connectOptions.secure) {
            m_wss->close(websocket::close_code::normal, ec);
        } else {
            m_ws->close(websocket::close_code::normal, ec);
        }
    
        m_ios.stop();
        m_iosThread.join();

        return ec;
    }

    template <typename EmitData>
    void emit(
        const std::string& eventName, const EmitData& data, const ResponseHandler respHandler = 0, 
        const bool noTimeout = false)
    {

        //  :TODO: should we connect if not already connected here? https://github.com/SocketCluster/socketcluster-client/blob/01a66770ea74b0f6185d7c59ea64b3d8bef078c6/lib/scsocket.js#L680

        auto self(shared_from_this());

        //  dispatch to our io thread
        m_ios.dispatch( [ self, this, eventName, data, respHandler, noTimeout ]() {

            json payload = {
                { "event",  eventName },
                { "data",   data }
            };

            if(respHandler) {
                const CallId cid = m_nextCallId++;
                
                payload["cid"] = cid;

                ResponseItem respItem( { respHandler, nullptr } );

                if(!noTimeout) {
                    //
                    //  If our event is not ACK'd by the server within |ackTimeout| we will
                    //  respond to the handler with a timeout error
                    //
                    respItem.ackTimer.reset(
                        new boost::asio::deadline_timer(m_ios, boost::posix_time::seconds(m_connectOptions.ackTimeout))
                    );

                    respItem.ackTimer->async_wait( [ self, this, cid ](const boost::system::error_code& ec) {
                        if(ec) {
                            //  likely canceled
                            return;
                        }

                        handleEmitAckTimeout(cid);          
                    });
                }

                m_pendingResponses[cid] = respItem;
            }

            m_outQueue.push(payload);
        });
    }

    void handleEmitAckTimeout(const CallId cid) {
        try {
            const ResponseItem respItem = m_pendingResponses.at(cid);
            m_pendingResponses.erase(cid);          
        
            std::stringstream ackTimeoutErrMsg;
            ackTimeoutErrMsg << "no ack for call id (cid) " << std::dec << cid;

            const json errorInfo = {
                { "error", {
                    { "message", ackTimeoutErrMsg.str() }
                }}
            };

            respItem.handler(make_error_code(ack_timeout), errorInfo);
        } catch(std::out_of_range) {
        }       
    }

    template <size_t HandlerId, typename ...Args>
    boost::signals2::connection on(Args&& ...args) {
        return std::get<HandlerId>(m_eventTable).connect(std::forward<Args>(args)...);
    }

    SCChannelPtr subscribe(const std::string& channelName, const ChannelSubscriptionOptions& channelSubOptions = ChannelSubscriptionOptions()) {
        //  :TODO: support waitForAuth - see https://github.com/SocketCluster/socketcluster-client/blob/01a66770ea74b0f6185d7c59ea64b3d8bef078c6/lib/scsocket.js

        SCChannelPtr channel;

        try {
            channel = m_channels.at(channelName);
        } catch(std::out_of_range) {            
            channel.reset(new SCChannel(channelName, shared_from_this()));
            m_channels[channelName] = channel;
        }

        if(ChannelState::UNSUBSCRIBED == channel->getState()) {
            channel->m_state = ChannelState::PENDING;
            tryChannelSubscribe(channel, channelSubOptions);
        }

        return channel;
    }

    void unsubscribe(const std::string& channelName) {
        try {
            const auto channel = m_channels.at(channelName);

            if(ChannelState::UNSUBSCRIBED != channel->getState()) {
                triggerChannelUnsubscribe(channel);
                sendChannelUnsubscribe(channel);
            }
        } catch(std::out_of_range) {
        }   
    }

    void destroyChannel(const std::string& channelName) {
        try {
            auto channel = m_channels.at(channelName);
            
            channel->unwatch();
            channel->unsubscribe();

            m_channels.erase(channelName);
        } catch(std::out_of_range) {            
        }
    }

    void unwatch(const std::string& channelName) {
        try {
            auto channel = m_channels.at(channelName);
            channel->unwatch();
        } catch(std::out_of_range) {            
        }
    }

    void unwatch(const std::string& channelName, const boost::signals2::connection& conn) {
        try {
            auto channel = m_channels.at(channelName);
            channel->unwatch(conn);
        } catch(std::out_of_range) {            
        }
    }

    boost::system::error_code disconnect() {
        boost::system::error_code ec;

        if(m_connectOptions.secure) {
            m_wss->close(websocket::close_code::normal, ec);
        } else {
            m_ws->close(websocket::close_code::normal, ec);
        }

        //  :TODO: why do we get an error here? Needs investigation
        return ec;
    }

    json const& getAuthToken() const { return m_authToken; }
    std::string const& getSignedAuthToken() const { return m_signedAuthToken; }
    
    State getState() const { return m_state; }
    AuthState getAuthState() const { return m_signedAuthToken.empty() ? AuthState::UNAUTHENTICATED : AuthState::AUTHENTICATED; }

    ChannelSubscriptions const& getChannels() const { return m_channels; }

    boost::asio::io_service& getIoService() { return m_ios; }

    ConnectOptions const& getConnectOptions() const { return m_connectOptions; }
private:    
    enum class ProtocolEvent {
        UNKNOWN,

        PUBLISH,
        REMOVE_TOKEN,
        SET_TOKEN,
        EVENT,
        IS_AUTHENTICATED,
        ACK_RECEIVE
    };  

    typedef std::queue<json> OutQueue;

    struct ResponseItem {
        ResponseHandler                                 handler;
        std::shared_ptr<boost::asio::deadline_timer>    ackTimer;
    };

    typedef boost::unordered_map<CallId, ResponseItem> PendingResponses;

    //  these MUST be in the order of EventHandlerIds
    typedef std::tuple<
        EventHandlerRaw,
        EventHandlerError,
        EventHandlerConnecting,
        EventHandlerConnect,
        EventConnectAbort,
        EventDisconnect,
        EventHandlerAuthenticate,
        EventHandlerAuthTokenChange,
        EventHandlerDeauthenticate,
        EventHandlerSubscribe,
        EventHandlerSubscribeFail,
        EventHandlerSubscriptionStateChange,
        EventHandlerUnsubscribe,
        EventHandlerEmit
    > EventTable;

    static const uint32_t RECONENCT_DELAY_INVALID   = 0xffffffff;

    typedef websocket::stream<tcp::socket>              WebSocket;
    typedef websocket::stream<ssl::stream<tcp::socket>> SecureWebSocket;    
    typedef std::unique_ptr<WebSocket>                  WebSocketPtr;
    typedef std::unique_ptr<SecureWebSocket>            SecureWebSocketPtr;

    State                               m_state;
    boost::asio::io_service             m_ios;
    ConnectOptions                      m_connectOptions;
    tcp::resolver                       m_resolver;
    std::shared_ptr<ssl::context>       m_sslContext;
    //  :TODO: this is super ugly: It would be nice to have a single m_ws e.g. in a variant. This has proven problematic however.
    //  ...templating is complex in that classes need to ref SCSocket & we want this to be switchable at runtime
    WebSocketPtr                        m_ws;
    SecureWebSocketPtr                  m_wss;
    boost::beast::multi_buffer          m_buffer;
    CallId                              m_nextCallId;
    OutQueue                            m_outQueue;
    std::string                         m_currentOutBuffer;
    PendingResponses                    m_pendingResponses;
    boost::thread                       m_iosThread;
    EventTable                          m_eventTable;
    std::string                         m_signedAuthToken;
    json                                m_authToken;
    ChannelSubscriptions                m_channels;
    uint32_t                            m_connectAttempts;
    uint32_t                            m_pingTimeout;
    boost::asio::deadline_timer         m_pingTimeoutTimer;

    void resetState() {
        m_state         = State::CONNECTING;
        m_nextCallId    = 1;

        resetPingTimer(true);
    }

    template<typename SocketType>
    void setPerMessageDeflate(SocketType& s) {
        s->set_option(m_connectOptions.perMessageDeflateOpts);
    }

    boost::system::error_code internalClose(
        const websocket::close_code code = websocket::close_code::normal)
    {
        boost::system::error_code ec;

        resetPingTimer(true);   //  cancel only

        if(State::OPEN == m_state) {
            m_state = State::CLOSED;

            if(m_connectOptions.secure) {
                m_wss->close(code, ec);
            } else {
                m_ws->close(code, ec);
            }
        }

        clearIoWriteQueue();
        suspendChannelSubscriptions();

        return ec;
    }

    void startConnect() {
        resetState();
        
        triggerEvent<ConnectingEvent>();

        m_resolver.async_resolve(
            { m_connectOptions.host, m_connectOptions.port },
            std::bind(&SCSocket::resolveHandler, shared_from_this(), std::placeholders::_1, std::placeholders::_2)
        );
    }

    void clearIoWriteQueue() {
        OutQueue().swap(m_outQueue);
    }

    void resetPingTimer(const bool cancelOnly = false) {
        m_pingTimeoutTimer.cancel();

        if(!cancelOnly) {
            auto self(shared_from_this());

            //  set updated pingTimeout
            m_pingTimeoutTimer.expires_from_now(boost::posix_time::milliseconds(m_pingTimeout));

            m_pingTimeoutTimer.async_wait( [ self, this ](const boost::system::error_code& ec) {
                if(ec) {
                    //  likely canceled
                    return;
                }

                if(m_connectOptions.secure) {
                    m_wss->close(websocket::close_code::protocol_error);
                } else {
                    m_ws->close(websocket::close_code::protocol_error);
                }
            });
        }
    }

    void tryChannelSubscribe(SCChannelPtr channel, const ChannelSubscriptionOptions& channelSubOptions) {

        const bool meetsRequirements = !channelSubOptions.waitForAuth || AuthState::AUTHENTICATED == getAuthState();

        if(State::OPEN == m_state && meetsRequirements) {   //  :TODO: we MUST ensure no other pending subscriptions are also going on here - there can only be one

            //  :TODO: need internal emit that waits for AUTH

            //
            //  We need to send a subscribe event to the server
            //
            json channelSubData = {
                { "channel",    channel->getName() }
            };
    
            if(!channelSubOptions.data.empty()) {
                channelSubData["data"] = channelSubOptions.data;
            }
    
            auto self(shared_from_this());

            emit("#subscribe", channelSubData, [ self, this, channel, channelSubOptions ](boost::system::error_code ec, const json&) {
                if(ec) {
                    return triggerChannelSubscribeFail(channel, ec, channelSubOptions);
                }

                triggerChannelSubscribe(channel, channelSubOptions);
            });
        }
    }

    void triggerChannelSubscribeFail(
        SCChannelPtr channel, const boost::system::error_code& ec, ChannelSubscriptionOptions channelSubOptions)
    {
        const bool meetsRequirements = !channelSubOptions.waitForAuth || AuthState::AUTHENTICATED == getAuthState();

        if(ChannelState::UNSUBSCRIBED != channel->getState() && meetsRequirements) {
            channel->m_state = ChannelState::UNSUBSCRIBED;

            //  channel
            channel->triggerEvent<SCChannel::SubscribeFailEvent>(channel->getName(), ec);

            //  socket
            triggerEvent<SubscribeFailEvent>(channel->getName(), ec);
        }
    }

    void triggerChannelSubscribe(SCChannelPtr channel, ChannelSubscriptionOptions channelSubOption) {
        const ChannelState oldState = channel->getState();

        if(ChannelState::SUBSCRIBED == oldState) {
            return;
        }

        channel->m_state = ChannelState::SUBSCRIBED;

        //  :TODO: JS version passes along channelSubOptions here as well
        const ChannelStateData stateData = { channel->getName(), oldState, channel->getState() };

        //  channel
        channel->triggerEvent<SCChannel::SubscriptionStateChangeEvent>(stateData);
        channel->triggerEvent<SCChannel::SubscribeEvent>(channel->getName());

        //  socket
        triggerEvent<SubscriptionStateChangeEvent>(stateData);
        triggerEvent<SubscribeEvent>(channel->getName());
    }

    void triggerChannelUnsubscribe(
        SCChannelPtr channel, const ChannelState newState = ChannelState::UNSUBSCRIBED)
    {
        const ChannelState oldState = channel->getState();

        channel->m_state = newState;

        cancelPendingSubscriberCallback(channel);

        if(ChannelState::SUBSCRIBED == oldState) {
            const ChannelStateData stateData( { channel->getName(), oldState, newState } );

            //  channel
            channel->triggerEvent<SCChannel::SubscriptionStateChangeEvent>(stateData);
            channel->triggerEvent<SCChannel::UnsubscribeEvent>(channel->getName());

            //  socket
            triggerEvent<SubscriptionStateChangeEvent>(stateData);
            triggerEvent<UnsubscribeEvent>(channel->getName());
        }
    }

    void sendChannelUnsubscribe(SCChannelPtr channel) {
        //  :TODO: If *our* state is not OPEN, do nothing

        cancelPendingSubscriberCallback(channel);

        //  :TODO: pass noTimeout option
        emit("#unsubscribe", channel->getName());
    }

    void cancelPendingSubscriberCallback(SCChannelPtr channel) {
        //  :TODO: if we have any pending subscriber callback for this channel, cancel it now

    }

    void suspendChannelSubscriptions() {
        ChannelState newState;

        for(const auto& sub : m_channels) {
            const ChannelState state = sub.second->getState();

            if(ChannelState::SUBSCRIBED == state || ChannelState::PENDING == state) {
                newState = ChannelState::PENDING;
            } else {
                newState = ChannelState::UNSUBSCRIBED;
            }

            triggerChannelUnsubscribe(sub.second, newState);
        }
    }

    void ioErrorHandler(const boost::system::error_code& ec) {
        if(boost::asio::error::operation_aborted == ec || boost::asio::error::eof == ec) {
            return closeHandler(ec, false);
        }

        //  :TODO: Handle!
        std::cout << "Unknown error: " << ec << std::endl << std::flush;
    }

    void closeHandler(const boost::system::error_code& ec, const bool isConnectionAbort) {
        internalClose();

        /*
        m_state = State::CLOSED;
        
        clearIoWriteQueue();
        suspendChannelSubscriptions();
        */

        //
        //  An boost::asio::error::operation_aborted reason is treated as a purposful
        //  disconnect; we will not attempt auto-reconnect
        //
        if(boost::asio::error::operation_aborted != ec && m_connectOptions.autoReconnect) {
            //  :TODO: when do we not want to try to reconnect? / when do we want to ignore delay?
            //  see https://github.com/SocketCluster/socketcluster-client/blob/01a66770ea74b0f6185d7c59ea64b3d8bef078c6/lib/scsocket.js#L580

            tryReconnect();
        }

        if(isConnectionAbort) {
            triggerEvent<ConnectAbortEvent>(ec);
        } else {
            triggerEvent<DisconnectEvent>(ec);
        }       
    }

    void tryReconnect(const uint32_t initialDelay = RECONENCT_DELAY_INVALID) {
        const uint32_t exponent = m_connectAttempts++;

        uint32_t timeout;

        if(RECONENCT_DELAY_INVALID == initialDelay || exponent > 0) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_real_distribution<> dis(0, 1);

            const uint32_t initialTimeout = std::round( 
                m_connectOptions.autoReconnectOptions.initialDelay + m_connectOptions.autoReconnectOptions.randomness * dis(gen)
            );

            timeout = std::round(initialTimeout * std::pow(m_connectOptions.autoReconnectOptions.multiplier, exponent));
        } else {
            timeout = initialDelay;
        }

        if(timeout > m_connectOptions.autoReconnectOptions.maxDelay) {
            timeout = m_connectOptions.autoReconnectOptions.maxDelay;
        }

        //  :TODO: clear any existing timer - use m_reconnectTimer for e.g.

        auto self(shared_from_this());

        std::shared_ptr<boost::asio::deadline_timer> timer(new boost::asio::deadline_timer(m_ios, boost::posix_time::milliseconds(timeout)));
        timer->async_wait( [ self, this, timer ](const boost::system::error_code& ec) {
            if(ec) {
                //  :TODO: handle |ec| here
                return;
            }

            startConnect();
        });
    }

    ProtocolEvent getEventType(const json& payload) const {
        try {
            std::string const& eventName = payload.at("event");

            if("#publish" == eventName) {
                return ProtocolEvent::PUBLISH;
            } else if("#removeAuthToken" == eventName) {
                return ProtocolEvent::REMOVE_TOKEN;
            } else if("#setAuthToken" == eventName) {
                return ProtocolEvent::SET_TOKEN;
            } else {
                return ProtocolEvent::EVENT;
            }
        } catch(std::out_of_range) {
            //  fall through
        }

        if(1 == payload.value("rid", 0)) {
            return ProtocolEvent::IS_AUTHENTICATED;
        }

        return ProtocolEvent::ACK_RECEIVE;
    }

    template<size_t HandlerId, typename ...Args>
    void triggerEvent(Args&& ...args) {
        (std::get<HandlerId>(m_eventTable))(std::forward<Args>(args)...);
    }

    void ioThread() {
        m_ios.run();
    }

    void placeNextWriteQueueItemInPayload() {
        const json obj = m_outQueue.front();
        m_outQueue.pop();

        m_currentOutBuffer = m_connectOptions.codecEngine ?
            m_connectOptions.codecEngine->encode(obj) :
            obj.dump()
            ;
    }

    void ioPumpWrite() {
        if(m_outQueue.empty()) {
            //  nothing to write - try to read
            return ioPumpReadSome();
        }

        placeNextWriteQueueItemInPayload();

        if(m_connectOptions.secure) {
            m_wss->async_write(
                boost::asio::buffer(m_currentOutBuffer),
                std::bind(&SCSocket::pumpWriteHandler, shared_from_this(), std::placeholders::_1)
            );
        } else {
            m_ws->async_write(
                boost::asio::buffer(m_currentOutBuffer),
                std::bind(&SCSocket::pumpWriteHandler, shared_from_this(), std::placeholders::_1)
            );
        }
    }

    void pumpWriteHandler(boost::system::error_code ec) {
        if(ec) {
            return ioErrorHandler(ec);
        }

        return ioPumpWrite();   //  write more if we can
    }

    void ioPumpReadSome() {

        if(m_connectOptions.secure) {
            m_wss->async_read_some(
                m_buffer,
                0,  //  :TODO: make this part of options
                std::bind(&SCSocket::readSomeHandler, shared_from_this(), std::placeholders::_1)
            );
        } else {
            m_ws->async_read_some(
                m_buffer,
                0,  //  :TODO: make this part of options
                std::bind(&SCSocket::readSomeHandler, shared_from_this(), std::placeholders::_1)
            );
        }
    }

    inline bool isCurrentMessageComplete() const {
        return m_connectOptions.secure ? 
            m_wss->is_message_done() : 
            m_ws->is_message_done()
            ;
    }

    void readSomeHandler(boost::system::error_code ec) {
        if(ec) {
            return ioErrorHandler(ec);
        }

        if(0 == m_buffer.size()) {
            return ioPumpWrite();
        }

        if(!isCurrentMessageComplete()) {
            return ioPumpReadSome();
        }

        //  "raw" event
        triggerEvent<RawEvent>(m_buffer);
        
        const auto bufferData = m_buffer.data();

        //
        //  Handle SocketCluster.io built in ping/pong (#1/#2)
        //
        if(2 == boost::asio::buffer_size(bufferData)) {
            std::array<char, 2> check;
            boost::asio::buffer_copy(boost::asio::buffer(&check, 2), bufferData);
            
            if('#' == check[0] && '1' == check[1]) {
                m_buffer.consume(m_buffer.size());  //  consume ping

                //  (re)start ping timer
                resetPingTimer();

                if(m_connectOptions.secure) {
                    m_wss->async_write(
                        boost::asio::buffer("#2"),
                        std::bind(&SCSocket::pumpWriteHandler, shared_from_this(), std::placeholders::_1)
                    );
                } else {
                    m_ws->async_write(
                        boost::asio::buffer("#2"),
                        std::bind(&SCSocket::pumpWriteHandler, shared_from_this(), std::placeholders::_1)
                    );
                }

                return; //  nothing further to do with a ping
            }
        }

        //  :TODO: there is VERY likely a more optimized way to do this -- see buffer views/etc.
        std::string buf;
        {
            std::stringstream temp;
            temp << boost::beast::buffers(bufferData);
            buf = temp.str();
        }

        //  we've consumed all of the current message
        m_buffer.consume(m_buffer.size());
    
        json payload;
        try {
            payload = m_connectOptions.codecEngine ?
                m_connectOptions.codecEngine->decode(buf) :
                json::parse(buf)
                ;

            if(!payload.is_object()) {
                triggerEvent<ErrorEvent>(make_error_code(protocol_error));
                return ioPumpWrite();
            }
        } catch(std::invalid_argument& ia) {
            triggerEvent<ErrorEvent>(make_error_code(json_parse_failure));
            return ioPumpWrite();
        }

        const ProtocolEvent eventType = getEventType(payload);      
        switch(eventType) {
            case ProtocolEvent::IS_AUTHENTICATED :
                //  like JS version, we emit when the handshake is complete.
                triggerEvent<ConnectEvent>(payload);
                break;

            case ProtocolEvent::PUBLISH :
                try {
                    json const& data                = payload.at("data");
                    std::string const& channelName  = data.at("channel");
                    json const& innerData           = data.at("data");

                    try {
                        auto channel = m_channels.at(channelName);
                        channel->triggerEvent<SCChannel::ChannelEvent>(innerData);
                    } catch(std::out_of_range) {
                        //  :TODO: anything?
                    }

                } catch(std::out_of_range) {
                    triggerEvent<ErrorEvent>(make_error_code(protocol_error));
                }
                break;

            case ProtocolEvent::REMOVE_TOKEN :
                m_signedAuthToken   = detail::EMPTY_STRING;
                m_authToken         = json::object();
                
                //  :TODO: should m_pingTimeout reset to ackTimeout ?

                triggerEvent<DeauthenticateEvent>();
                break;

            case ProtocolEvent::SET_TOKEN :
                try {
                    json const& data            = payload.at("data");
                    std::string const& jwtToken = data.at("token");

                    //  update pingTimeout if possible
                    m_pingTimeout               = data.value("pingTimeout", m_pingTimeout);
                    
                    //
                    //  Raw JWT should be in header.payload.signature format
                    //
                    //  :TODO: Additional validation? - see https://tools.ietf.org/html/rfc7519#section-7.2
                    std::vector<std::string> jwtParts;
                    boost::split(jwtParts, jwtToken, boost::is_any_of("."));

                    if(3 == jwtParts.size()) {
                        try {
                            //  payload is base64 -- decode that first.
                            typedef boost::archive::iterators::transform_width<
                                boost::archive::iterators::binary_from_base64<char *>,
                                8,
                                6
                            > Base64;

                            const std::string encoded = jwtParts[1];
                            const std::string jwtPayload(Base64(&encoded[0]), Base64(&encoded[0] + encoded.length()));

                            //  save to know if we're initially authing
                            const std::string oldJwtToken = m_signedAuthToken;
                            
                            m_authToken         = json::parse(jwtPayload);
                            m_signedAuthToken   = jwtToken;

                            if(oldJwtToken.empty()) {
                                triggerEvent<AuthenticateEvent>(m_signedAuthToken);
                            }

                            triggerEvent<AuthTokenChangeEvent>(m_signedAuthToken);
                        } catch(std::invalid_argument) {
                            triggerEvent<ErrorEvent>(make_error_code(protocol_error));
                        }                           
                    } else {
                        //  :TODO: not a valid JWT -- what to do?
                    }

                } catch(std::out_of_range) {
                    triggerEvent<ErrorEvent>(make_error_code(protocol_error));
                }
                break;

            case ProtocolEvent::ACK_RECEIVE :
                {                   
                    const CallId rid = payload.value("rid", 0);

                    try {
                        const ResponseItem respItem = m_pendingResponses.at(rid);

                        //  cancel ACK timer, if any
                        if(respItem.ackTimer) {
                            respItem.ackTimer->cancel();
                        }

                        try {
                            json const& error = payload.at("error");

                            respItem.handler(
                                make_error_code(response_error),
                                error
                            );
                        } catch(std::out_of_range) {
                            respItem.handler(
                                boost::system::error_code(),
                                payload.value("data", json::object())   //  data is optional
                            );
                        }
                    } catch(std::out_of_range) {
                        triggerEvent<ErrorEvent>(make_error_code(unexpected_rid));
                    }
                }
                break;
            
            case ProtocolEvent::EVENT :
                try {
                    const CallId cid                = payload.value("cid", 0);
                    std::string const& eventName    = payload.at("event");
                    json const& eventData           = payload.at("data");
                    
                    //  if we have "cid", a response is requested
                    if(cid) {
                        auto self(shared_from_this());

                        (std::get<EmitEvent>(m_eventTable))(
                            eventName,
                            eventData,
                            [ self, this, cid ](const json& resp) {

                                const json emitRespPayload = {
                                    { "rid",        cid },
                                    { "data",       resp },
                                };

                                m_outQueue.push(emitRespPayload);
                            }
                        );
                    } else {
                        (std::get<EmitEvent>(m_eventTable))(
                            eventName,
                            eventData,
                            0   //  no resp handler
                        );
                    }               
                } catch(std::out_of_range) {
                    triggerEvent<ErrorEvent>(make_error_code(protocol_error));
                }               
                break;

            default :
                std::cout << "unknown event read " << std::dec << (int)eventType << std::endl << payload << std::endl;
                break;
        }

        return ioPumpWrite();
    }

    void resolveHandler(boost::system::error_code ec, tcp::resolver::iterator resolveIter) {
        if(ec) {
            return closeHandler(ec, true);
        }

        if(m_connectOptions.secure) {
            //
            //  For TLS/SSL we have an additional handshake step
            //
            boost::asio::async_connect(
                m_wss->next_layer().next_layer(),
                resolveIter,
                std::bind(&SCSocket::secureConnectHandler, shared_from_this(), std::placeholders::_1)
            );
        } else {
            boost::asio::async_connect(
                m_ws->next_layer(), 
                resolveIter,
                std::bind(&SCSocket::connectHandler, shared_from_this(), std::placeholders::_1)
            );
        }
    }

    void secureConnectHandler(boost::system::error_code ec) {
        if(ec) {
            return closeHandler(ec, true);
        }

        m_wss->next_layer().async_handshake(
            ssl::stream_base::client,
            std::bind(&SCSocket::connectHandler, shared_from_this(), std::placeholders::_1)
        );
    }

    void connectHandler(boost::system::error_code ec) {
        if(ec) {
            return closeHandler(ec, true);
        }

        m_state = State::OPEN;

        if(m_connectOptions.secure) {
            performHandshake(m_wss);
        } else {
            performHandshake(m_ws);
        }
    }

    template <typename NextLayer>
    void performHandshake(std::unique_ptr<NextLayer>& s) {
        auto self(shared_from_this());

        s->async_handshake_ex(
            m_connectOptions.host,
            m_connectOptions.path,
            [ this, self ](boost::beast::websocket::request_type& req) {
                if(!m_connectOptions.userAgent.empty()) {
                    req.insert(boost::beast::http::field::user_agent, m_connectOptions.userAgent);
                }
            },
            std::bind(&SCSocket::initialHandshakeHandler, shared_from_this(), std::placeholders::_1)
        );
    }

    void initialHandshakeHandler(boost::system::error_code ec) {
        if(ec) {
            //  :TODO: emit error
            std::cout << "initialHandshakeHandler ec: " << ec.message() << std::endl << std::flush;
            return;
        }

        //
        //  Send out a #handshake. We can't use emit as it's a special case
        //
        const json handshakePayload = {
            { "event",  "#handshake" },
            { "data",   nullptr },
            //{ "data", {{ "authToken", nullptr }} },
            { "cid",    m_nextCallId++ }
        };

        m_outQueue.push(handshakePayload);

        return ioPumpWrite();
    }

    bool haveBinaryCodec() const {
        return m_connectOptions.codecEngine && m_connectOptions.codecEngine->isBinary();
    }
};


void SCChannel::unsubscribe() {
    m_socket->unsubscribe(m_name);
}

void SCChannel::destroy() {
    m_socket->destroyChannel(m_name);
}

class SocketClusterClientOptions {
public:
    ConnectOptions          connectOptions;
};

class SocketClusterClient
    : public std::enable_shared_from_this<SocketClusterClient>
{
protected:
    struct PrivateTag;

public: 
    typedef std::shared_ptr<SocketClusterClient> SocketClusterClientPtr;
    typedef std::shared_ptr<SCSocket> SCSocketPtr;

    //  force consumers to utilize create()
    explicit SocketClusterClient(const PrivateTag&) { }
    explicit SocketClusterClient(const PrivateTag&, const SocketClusterClientOptions& clientOptions)
        : m_clientOpts(clientOptions)
    {
    }

    template <typename... T>
    static SocketClusterClientPtr create(T&&...args) {
        return std::make_shared<SocketClusterClient>(PrivateTag{0}, std::forward<T>(args)...);
    }

    void shutdown() {
        for(auto& client : m_clientSockets) {
            client->close();
        }

        m_clientSockets.clear();
    }

    SCSocketPtr socket() { return socket(m_clientOpts.connectOptions); }
    
    SCSocketPtr socket(const ConnectOptions& connectOpts) {
        SCSocketPtr socket;

        if(connectOpts.secure) {                        
            socket = std::make_shared<SCSocket>(connectOpts);
        } else {
            socket = std::make_shared<SCSocket>(connectOpts);
        }
        
        m_clientSockets.insert(socket);

        return socket;
    }
protected:
    struct PrivateTag {
        explicit PrivateTag(int) {}
    };

private:
    typedef std::set<SCSocketPtr> ClientSockets;

    SocketClusterClientOptions      m_clientOpts;
    ClientSockets                   m_clientSockets;
};

}   //  end scio_beast ns

#endif  //  SOCKETCLUSTER_IO_BEAST_H
