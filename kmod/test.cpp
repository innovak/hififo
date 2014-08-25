/* 
 * HIFIFO: Harmon Instruments PCI Express to FIFO
 * Copyright (C) 2014 Harmon Instruments, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <thread>
#include <iostream>

using namespace std;

class TimeIt {
  struct timespec start_time;
 public:
  TimeIt() {
    clock_gettime(CLOCK_REALTIME, &start_time);
  }
  double elapsed(void){
    struct timespec stop_time;
    clock_gettime(CLOCK_REALTIME, &stop_time);
    return (stop_time.tv_sec - start_time.tv_sec) + 1e-9 * (stop_time.tv_nsec - start_time.tv_nsec);  
  };
};

void* operator new(size_t sz) throw (std::bad_alloc)
{
  void* mem = malloc(sz);
  if (mem)
    return mem;
  else
    throw std::bad_alloc();
}


void operator delete(void* ptr) throw()
{
  free(ptr);
}

#define IOC_INFO 0x10
#define IOC_GET_TO_PC 0x11
#define IOC_PUT_TO_PC 0x12
#define IOC_GET_FROM_PC 0x13
#define IOC_PUT_FROM_PC 0x14
#define IOC_SET_TIMEOUT 0x15

#define min(x,y) ((x) > (y) ? (y) : (x))
#define max(x,y) ((x) < (y) ? (y) : (x))

struct fifodev
{
  int fd;
  uint64_t read_size;
  uint64_t write_size;
  uint64_t read_available;
  uint64_t write_available;
  uint64_t read_pointer;
  uint64_t write_pointer;
  uint64_t read_mask;
  uint64_t write_mask;
};

struct fifodev * fifo_open(const char * filename)
{
  uint64_t tmp[8];
  struct fifodev *f = (struct fifodev *) calloc(1, sizeof(struct fifodev));
  if(f == NULL)
    goto fail0;
  f->fd = open(filename, O_RDWR);
  if(f->fd < 0){
    perror(filename);
    goto fail1;
  }
  if(ioctl(f->fd, _IOR('f', IOC_INFO, uint64_t[8]), tmp) != 0){
    perror("device info ioctl failed");
    goto fail2;
  }
  f->read_size = tmp[0];
  f->write_size = tmp[1];
  f->read_pointer = tmp[2];
  f->write_pointer = tmp[3];
  //fprintf(stderr, "read_size = %ld, write_size = %ld\n", f->read_size, f->write_size);
  
  /*f->read_base = (uint8_t *) mmap(NULL, 2 * (f->read_size + f->write_size), PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
  if(f->read_base == MAP_FAILED) {
    perror("mmap failed");
    goto fail2;
  }
  */
  return f;
  //munmap(f->read_base, 2 * (f->read_size + f->write_size));
 fail2:
  close(f->fd);
 fail1:
  free(f);
 fail0:
  fprintf(stderr, "fifo_open(%s) failed\n", filename);
  return NULL;
}

void fifo_close(struct fifodev *f){
  //munmap(f->read_base, 2 * (f->read_size + f->write_size));
  close(f->fd);
  free(f);
}

/* 
 * returns a pointer to a buffer containing at least count
 * bytes from the FIFO or returns NULL if insufficient data
 * is available. Updates f->read_available, can 
 */
/*void * fifo_read_get(struct fifodev *f, uint64_t count){
  if(count < f->read_available)
    return f->read_base + f->read_pointer;
  unsigned long tmp = ioctl(f->fd, _IO('f', IOC_GET_TO_PC), count);
  if(tmp < 0){
    perror("hififo.c: fifo_read_get failed");
    return NULL;
  }
  f->read_available = tmp;
  if(tmp < count)
    return NULL;
  return f->read_base + f->read_pointer;
}

int fifo_read_free(struct fifodev *f, uint64_t count){
  f->read_available -= count;
  f->read_pointer += count;
  f->read_pointer &= f->read_mask;
  if(ioctl(f->fd, _IO('f', IOC_PUT_TO_PC), count) != 0){
    perror("hififo.c: fifo_read_free() failed");
    return -1;
  }
  return 0;
  }

void * fifo_write_get(struct fifodev *f, uint64_t count){
  if(count < f->write_available)
    return f->write_base + f->write_pointer;
  unsigned long tmp = ioctl(f->fd, _IO('f', IOC_GET_FROM_PC), count);
  if(tmp < 0){
    perror("hififo.c: fifo_write_get() failed");
    return NULL;
  }
  f->write_available = tmp;
  //fprintf(stderr, "fifo_write_get: %lx available\n", tmp);
  if(tmp < count)
    return NULL;
  return f->write_base + f->write_pointer;
}

uint64_t fifo_write_put(struct fifodev *f, uint64_t count){
  f->write_available -= count;
  f->write_pointer += count;
  f->write_pointer &= f->write_mask;
  if(ioctl(f->fd, _IO('f', IOC_PUT_FROM_PC), count) != 0){
    perror("hififo.c: fifo_write_put() failed");
    return -1;
  }
  return 0;
  }*/

uint64_t fifo_set_timeout(struct fifodev *f, uint64_t timeout){
  if(ioctl(f->fd, _IO('f', IOC_SET_TIMEOUT), timeout) != 0){
    perror("hififo.c: fifo_set_timeout() failed");
    return -1;
  }
  return 0;
}

void writer(struct fifodev *f, size_t count)
{
  size_t bs = 1024*512;
  ssize_t retval;
  uint64_t wcount = 0xDEADE00000000000;
  uint64_t *wbuf = (uint64_t *) mmap(NULL, bs*8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_HUGETLB, 1, 0);
  cerr << wbuf << " wbuf\n";
  if(wbuf == NULL){
    cerr << "mmap failed\n";
    return;
  }
  //new uint64_t[bs];
  TimeIt timer{};
  for(uint64_t i=0; i<count; i+=bs){
    //fprintf(stderr, "writer: i = %lx, rcount = %lx, available = %lx, wbuf = %lx\n", i, rcount, f->write_available, (uint64_t) wbuf);
    for(size_t j=0; j<bs; j++)
      wbuf[j] = wcount++;
    retval = write(f->fd, wbuf, bs*8);
    if((size_t) retval != bs*8){
      std::cerr << "failed to write, attempted " << 8*bs << " bytes, retval = " << retval << "\n";
      break;
    }
  }
  auto runtime = timer.elapsed();
  auto speed = count * 8.0e-6 / runtime;
  std::cerr << "wrote " << count*8 << " bytes in " << runtime << " seconds, " << speed << " MB/s\n";
  //delete[] wbuf;
  munmap(wbuf, bs);
};

void checker(struct fifodev *f, size_t count)
{
  uint64_t expected = 0;
  size_t bs = 1024*128;
  int prints = 0;
  ssize_t retval;
  uint64_t *buf = new uint64_t[bs];
  TimeIt timer{};
  for(uint64_t i=0; i<count; i+=bs){
    retval = read(f->fd, buf, bs*8);
    if((size_t) retval != bs*8){
      std::cerr << "failed to read, attempted " << 8*bs << " bytes, retval = " << retval << "\n";
      break;
    }
    if(i == 0)
      expected = buf[0];
    for(unsigned int j=0; j<bs; j++){
      if((expected != buf[j]))
	{
	  std::cerr << "error at " << i << " " << j << " rval = 0x" << std::hex << buf[j] << " expected = 0x" << expected << "\n" << std::dec;
	  prints++;
	}
      expected = buf[j] + 1; 
    }
  }
  double runtime = timer.elapsed();
  double speed = count * 8.0e-6 / runtime;
  std::cerr << "read " << count*8L << " bytes in " << runtime << " seconds, " << speed << " MB/s\n";
  delete[] buf;
}

int main ( int argc, char **argv )
{
  uint64_t length = 1048576L*128L;//128L*8L;
  struct fifodev *f2 = fifo_open("/dev/hififo_0_2");
  if(f2 == NULL)
    exit(EXIT_FAILURE);
  struct fifodev *f6 = fifo_open("/dev/hififo_0_6");
  if(f6 == NULL)
    exit(EXIT_FAILURE);
  struct fifodev *f0 = fifo_open("/dev/hififo_0_0");
  if(f0 == NULL)
    exit(EXIT_FAILURE);
  struct fifodev *f4 = fifo_open("/dev/hififo_0_4");
  if(f4 == NULL)
    exit(EXIT_FAILURE);

  writer(f2, length);
  checker(f6, length);
  
  std::cerr << "f0 -> f4\n";
  #pragma omp parallel sections
  {
    #pragma omp section
    {
      checker(f4, length);
    }
    #pragma omp section
    {
      writer(f0, length);
    }
  }

  std::cerr << "f2 -> f6\n";
  std::thread t_reader (checker, f6, length);
  std::thread t_writer (writer, f2, length);
  t_writer.join();
  t_reader.join();

  fifo_close(f0);
  fifo_close(f2);
  fifo_close(f4);
  fifo_close(f6);

  return 0;
}
