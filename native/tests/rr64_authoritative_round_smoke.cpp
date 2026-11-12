#include "rr64_authoritative_round.hpp"
#include <cstdlib>
#include <limits>
#include <vector>
#include <cstdio>
#include <source_location>
using namespace rr64::authority;
void check(bool v,std::source_location where=std::source_location::current()){
 if(!v){std::fprintf(stderr,"round check failed line %u\n",where.line());std::exit(1);}
}
InputBatch batch(ClientHistory &c){InputBatch b{};b.round=1;b.count=c.resend(b.commands);return b;}
int main(){
 HostRound host;check(host.reset(1,(1u<<14)-1));std::array<ClientHistory,14> clients;
 for(unsigned s=0;s<14;++s){clients[s].reset(1);Command c;check(clients[s].append(0x1001,s,0,c,action_eject));check(host.receive(s,batch(clients[s])));}
 Step step;check(host.begin(step));check(!host.begin(step));check(host.stamp().tick==0);
 for(unsigned s=0;s<14;++s){check(step.inputs[s].actions==action_eject);check(step.inputs[s].x==s);check(step.inputs[s].buttons==(s?1:0x1001));check(host.stamp().acknowledged[s]==0);}
 check(!host.finish(2));check(host.finish(1));check(!host.finish(1));
 for(auto a:host.stamp().acknowledged)check(a==1);
 // Retransmitted ACK-lost packets cannot replay a tap. Missing steps do not ACK.
 check(host.receive(1,batch(clients[1])));
 for(unsigned i=0;i<16;++i){check(host.begin(step));check(step.inputs[1].sequence==0 && step.inputs[1].actions==0);if((i+1)*16667>250000)check(step.inputs[1].buttons==0&&step.inputs[1].x==0);else check(step.inputs[1].buttons==1);check(host.finish(step.tick));}
 check(host.stamp().acknowledged[1]==1);
 // A bad final command must not install a valid first command from the packet.
 InputBatch bad{};bad.round=1;bad.count=2;bad.commands[0]={1,2,0,4,0};bad.commands[1]={2,3,0,5,0};
 check(!host.receive(1,bad));check(host.begin(step));check(step.inputs[1].sequence==0);check(host.finish(step.tick));
 bad.count=1;check(!host.receive(14,bad));bad.round=2;check(!host.receive(1,bad));
 // Delayed acknowledgement must not stop fresh controls reaching the host.
 ClientHistory delayed;delayed.reset(1);Command c;
 for(unsigned i=1;i<=40;++i)check(delayed.append(0,i,0,c));
 auto resend=batch(delayed);check(resend.count==16);check(resend.commands[0].sequence==1);check(resend.commands[7].sequence==8);check(resend.commands[8].sequence==33);check(resend.commands[15].sequence==40);
 check(!host.reset(0,3));check(host.reset(2,3));bad.round=1;check(!host.receive(1,bad));
 check(host.stamp().tick==0 && host.stamp().acknowledged[1]==0);
 // Sustained delayed/lost/reordered batches and delayed acknowledgements.
 // All14 streams must consume each sample exactly once, retaining the union
 // of transient edges when delayed samples share a native update.
 struct Delivery {unsigned due,slot;InputBatch batch;};
 struct Ack {unsigned due,slot;Stamp stamp;};
 std::vector<Delivery> deliveries;std::vector<Ack> acks;
 check(host.reset(1,(1u<<14)-1));for(auto &c:clients)c.reset(1);
 std::array<unsigned,14> committed{};
 for(unsigned t=1;t<=900;++t){
   for(unsigned s=0;s<14;++s){
     Command c;if(t<=600)check(clients[s].append(t%7==0,1,0,c,t%13==0?action_eject:0));
     if(clients[s].pending() && (t+s)%7!=0)deliveries.push_back({t+4+(t+s)%5,s,batch(clients[s])});
   }
   for(auto i=deliveries.begin();i!=deliveries.end();) {
     if(i->due<=t){check(host.receive(i->slot,i->batch));i=deliveries.erase(i);}else ++i;
   }
   check(host.begin(step));
   for(unsigned s=0;s<14;++s)if(step.inputs[s].sequence){
     check(step.inputs[s].sequence>committed[s]);unsigned actions=0,presses=0;
     for(unsigned i=committed[s]+1;i<=step.inputs[s].sequence;++i){if(i%13==0)actions|=action_eject;if(i%7==0)presses|=1;}
     check(step.inputs[s].actions==actions && step.presses[s]==presses);
     committed[s]=step.inputs[s].sequence;
   }
   check(host.finish(step.tick));
   if(t%3==0)for(unsigned s=0;s<14;++s)acks.push_back({t+6,s,host.stamp()});
   for(auto i=acks.begin();i!=acks.end();) {
     if(i->due<=t){int state=0;check(clients[i->slot].reconcile(1,i->stamp.tick,i->stamp.acknowledged[i->slot],0,state,[](int &,const Command &,bool){return true;}));i=acks.erase(i);}else ++i;
   }
 }
 for(unsigned s=0;s<14;++s)check(committed[s]==600 && clients[s].pending()==0);
}
