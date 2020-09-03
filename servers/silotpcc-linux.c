#include "common.h"
#include "silotpcc.h"

void process_request(void)
{
  silotpcc_exec_one(thread_no);
}

void init_thread(void)
{
  silotpcc_init_thread(thread_no);
}

int main(int argc, char *argv[])
{
  //memset(dsum, 0x0, sizeof(dsum));
  //memset(freqsum, 0x0, sizeof(freqsum));
  //memset(workerloop, 0x0, sizeof(workerloop));
  
  init_linux();
  silotpcc_init(nr_cpu, 40l * (1 << 30));
  start_linux_server();
  
  return 0;
}
