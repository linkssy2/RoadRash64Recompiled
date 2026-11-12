// Exercise production transport directly with a real loopback UDP peer and a
// controlled clock. No game, firewall edits or internet service required.
#include "../src/rr64_netplay.cpp"
#include <cstdlib>
using namespace rr64::netplay;
static void require(bool condition,const char* message){if(!condition){std::fprintf(stderr,"Connection FAIL: %s\n",message);std::exit(1);}}
int main(){
    std::uint16_t port=6464;std::string host,error;
    require(parse_connection_address(" 127.0.0.1:26468 \r\n",port,host,error)&&port==26468&&host=="127.0.0.1","pasted endpoint");
    for(const auto* invalid:{"","  ","127.0.0.1:0","127.0.0.1:65536","127.0.0.1:abc","::1","http://host","bad host"}){
        port=6464;require(!parse_connection_address(invalid,port,host,error),"invalid address refused");
    }
    require(start_winsock_locked(),"winsock");
    SOCKET server=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);require(server!=INVALID_SOCKET,"test UDP server");
    sockaddr_in endpoint{};endpoint.sin_family=AF_INET;endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    require(bind(server,reinterpret_cast<sockaddr*>(&endpoint),sizeof(endpoint))==0,"server bind");
    int length=sizeof(endpoint);getsockname(server,reinterpret_cast<sockaddr*>(&endpoint),&length);
    u_long nonblocking=1;ioctlsocket(server,FIONBIO,&nonblocking);
    Config config{};config.mode=Mode::Join;config.host_address=" 127.0.0.1:"+std::to_string(ntohs(endpoint.sin_port))+" ";
    config.port=6464;configure(config);require(g_session.phase==Phase::Connecting,"joining begins");
    const auto start=g_session.connect_started;
    for(unsigned i=0;i<3;++i)service_locked(start+std::chrono::milliseconds(i*500));
    require(g_session.hello_attempts==3 && g_session.local_slot==kInvalidSlot,"lost welcomes retry");
    sockaddr_in client{};int clientLength=sizeof(client);std::array<char,2048> bytes{};unsigned hellos=0;
    while(recvfrom(server,bytes.data(),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&client),&clientLength)>0)++hellos;
    require(hellos==3,"three real UDP hellos");
    WelcomePacket welcome{};welcome.header.size=sizeof(welcome);welcome.header.type=PacketType::Welcome;welcome.header.session=12345;welcome.assigned_slot=1;
    require(sendto(server,reinterpret_cast<const char*>(&welcome),sizeof(welcome),0,reinterpret_cast<sockaddr*>(&client),sizeof(client))==sizeof(welcome),"welcome send");
    pump_receive_locked(start+std::chrono::seconds(2));require(g_session.phase==Phase::Lobby&&g_session.local_slot==1,"real welcome accepted");
    g_session.phase=Phase::Race;
    handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&welcome),sizeof(welcome),endpoint,start+std::chrono::seconds(3));
    require(g_session.phase==Phase::Race,"late welcome cannot rewind race");
    service_locked(start+std::chrono::seconds(8));require(g_session.socket==INVALID_SOCKET&&g_session.message.find("lost")!=std::string::npos,"dead host timeout");
    configure(config);service_locked(g_session.connect_started+std::chrono::seconds(16));
    require(g_session.socket==INVALID_SOCKET&&g_session.phase==Phase::Offline&&g_session.message.find("15 seconds")!=std::string::npos,"no infinite connecting");
    for(auto reason:{RejectReason::Full,RejectReason::Busy,RejectReason::Version}){
        configure(config);ConnectRejectPacket rejection{};rejection.header.type=PacketType::ConnectReject;rejection.header.size=sizeof(rejection);rejection.reason=reason;
        auto wrong=endpoint;wrong.sin_port=htons(ntohs(endpoint.sin_port)+1);
        handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&rejection),sizeof(rejection),wrong,Clock::now());
        require(g_session.phase==Phase::Connecting,"foreign rejection ignored");
        handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&rejection),sizeof(rejection),endpoint,Clock::now());
        require(g_session.socket==INVALID_SOCKET&&g_session.phase==Phase::Offline,"host rejection closes attempt");
    }
    configure(config);sockaddr_in bound{};length=sizeof(bound);getsockname(g_session.socket,reinterpret_cast<sockaddr*>(&bound),&length);bound.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    welcome.header.version=5;
    sendto(server,reinterpret_cast<const char*>(&welcome),sizeof(welcome),0,reinterpret_cast<sockaddr*>(&bound),sizeof(bound));
    pump_receive_locked(Clock::now());require(g_session.message.find("versions differ")!=std::string::npos,"wire version mismatch explained");
    shutdown();closesocket(server);
    config.mode=Mode::Host;config.port=26469;configure(config);require(get_status().active,"host bind");
    SOCKET contender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);BOOL reuse=TRUE;setsockopt(contender,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<char*>(&reuse),sizeof(reuse));
    endpoint.sin_addr.s_addr=htonl(INADDR_ANY);endpoint.sin_port=htons(config.port);
    require(bind(contender,reinterpret_cast<sockaddr*>(&endpoint),sizeof(endpoint))==SOCKET_ERROR,"second host cannot steal bound port");
    closesocket(contender);shutdown();config.port=0;configure(config);require(!get_status().active,"invalid port does not silently use default");
    std::puts("Connection: endpoint parsing, real UDP retry/welcome, delayed welcome, dead/no host, reject provenance, wire mismatch and exclusive host port passed.");
}

