#include "model.h"
#include "worker_wire.h"
#include <sys/prctl.h>
#include <signal.h>
#include <cstdlib>
#include <unistd.h>
#include <malloc.h>
int main(int argc,char **argv){
 if(argc!=4)return 1;pid_t parent=pid_t(std::strtol(argv[3],nullptr,10));if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent)return 2;prctl(PR_SET_NAME,"lyra-stem-work",0,0,0);mallopt(M_DECAY_TIME,0);
 lyra_stems::Model model;uint32_t handshake=0;if(model.load(argv[1],argv[2],1))handshake=lyra_stems::wire_magic;if(!lyra_stems::transfer(1,&handshake,sizeof(handshake),true) || !handshake)return 3;
 std::vector<float> input(size_t(LYRA_STEM_INPUT_FRAMES)*2);for(;;){uint32_t size=0;if(!lyra_stems::transfer(0,&size,sizeof(size),false,-1))return 0;if(size!=input.size()*sizeof(float))return 4;if(!lyra_stems::transfer(0,input.data(),size,false))return 0;uint32_t result=model.process(input.data(),input.size())?size:0;if(!lyra_stems::transfer(1,&result,sizeof(result),true))return 0;if(!result)return 5;const auto &out=model.output();if(!lyra_stems::transfer(1,const_cast<float*>(out.data()),result,true))return 0;}
}
