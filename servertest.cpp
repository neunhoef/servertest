// Test program for server infrastructure

#include <iostream>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <string>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <xmmintrin.h>

std::string pretty(uint64_t u) {
  if (u == 0) {
    return "0";
  }
  std::string r;
  int c = 0;
  while (u > 0) {
    if (c == 3) {
      r = "," + r;
      c = 0;
    }
    r = std::to_string(u % 10) + r;
    u /= 10;
    ++c;
  }
  return r;
}

class Work {
  size_t howmuch;
  size_t sum;

 public:
  Work(size_t h) : howmuch(h), sum(0) {
  }

  void dowork() {
    size_t s = 0;
    for (size_t i = 0; i < howmuch; ++i) {
      s += i * i;
    }
    sum += s;
  }

  size_t get() {
    return sum;
  }

};

double workTime = 0.0;   // time in seconds for one piece of work, will be
                         // gauged at beginning of program

void singleThread(Work* work, std::atomic<int>* stop, uint64_t* count) {
  // simply work until stop is signalled:
  uint64_t c = 0;
  size_t perRound = ceill(1e-5 / workTime);
  while (stop->load() == 0) {
    for (size_t i = 0; i < perRound; ++i) {
      work->dowork();
      ++c;
    }
  }
  *count = c;
}

void multipleThreads(Work* work, std::mutex* mutex, std::atomic<int>* stop,
                     uint64_t* count) {
  // simply work until stop is signalled, but with a mutex:
  uint64_t c = 0;
  size_t perRound = ceill(1e-5 / workTime);
  while (stop->load() == 0) {
    for (size_t i = 0; i < perRound; ++i) {
      {
        std::unique_lock<std::mutex> guard(*mutex);
        work->dowork();
      }
      ++c;
    }
  }
  *count = c;
}

class Server {
 public:
  struct alignas(128) Client {
    std::atomic<uint32_t> inTick;  // starts as 0, an increase means that
                                   // a new job has to be done
    uint32_t what;    // indicates what to do
    Work* work;
    char padding[120 - sizeof(Work*)];
    std::atomic<uint32_t> outTick;  // starts as 0, an increase means that
                                    // a new answer is there
    std::atomic<uint32_t> serverGone;
    char padding2[124];
    Client(Work* w) : inTick(0), what(0), work(w), outTick(0), serverGone(0) { }
  };

 private:
  std::mutex mutex;
  std::vector<Client*> newClients;
  std::vector<Client*> toRemove;
  std::atomic<uint32_t> changed;  // increase to make the server look at lists
  char padding[128];              // just to go to other cache line

  std::vector<Client*> clients;
  std::vector<uint32_t> ticks;
  std::atomic<uint32_t> stop;
  std::thread server;

 public:
  Server() 
    : changed(0), stop(0), server(&Server::run, this) { }

  ~Server() {
    stop = 1;
    server.join();
  }

  void registerClient(Client* c) {
    std::unique_lock<std::mutex> guard(mutex);
    newClients.push_back(c);
    ++changed;
  }

  void unregisterClient(Client* c) {
    {
      std::unique_lock<std::mutex> guard(mutex);
      toRemove.push_back(c);
      ++changed;
    }
    while (changed > 0) {}
  }

  void duplicatePointers() {
    size_t size = clients.size();
    if (size > 0) {
      for (size_t i = 0; i < size; ++i) {
        clients.push_back(clients[i]);   // Copy first to end
      }
    }
  }

  void removeDuplicatePointers() {
    size_t size = clients.size();
    if (size > 0) {  // Remove all copies of the pointers:
      for (size_t i = 0; i < size/2; ++i) {
        clients.pop_back();  // Remove copy
      }
    }
  }

  void run() {
    while (true) {
      // Usual work:
      size_t s = clients.size();
      if (s > 0) {  // s is always even!
        size_t ss = s >> 1;
        for (size_t i = 0; i < ss; ++i) {
          _mm_prefetch(clients[i+ss], _mm_hint::_MM_HINT_T0);
          uint32_t t = clients[i]->inTick.load(std::memory_order_relaxed);
          if (t != ticks[i]) {
            ticks[i] = t;
            clients[i]->work->dowork();
            clients[i]->outTick.store(t, std::memory_order_relaxed);
          }
        }
      }

      // Look after changes:
      if (changed.load(std::memory_order_relaxed) > 0) {
        // Mutex ensures memory barrier
        std::unique_lock<std::mutex> guard(mutex);
        removeDuplicatePointers();
        for (size_t i = 0; i < toRemove.size(); ++i) {
          for (size_t j = 0; j < clients.size(); ++j) {
            if (toRemove[i] == clients[j]) {
              clients[j] = clients.back();
              clients.pop_back();
              ticks[j] = ticks.back();
              ticks.pop_back();
              break;
            }
          }
        }
        toRemove.clear();
        for (size_t i = 0; i < newClients.size(); ++i) {
          clients.push_back(newClients[i]);
          ticks.push_back(0);
        }
        newClients.clear();
        duplicatePointers();
        changed.store(0, std::memory_order_relaxed);  // under the mutex!
      }

      // Stop?
      if (stop.load(std::memory_order_relaxed) > 0) {
        removeDuplicatePointers();
        if (clients.size() > 0) {
          for (size_t i = 0; i < clients.size(); ++i) {
            clients[i]->serverGone = 1;
          }
        }
        break;
      }
    }
  }
};

void clientThread(Server* server, Work* work, std::atomic<int>* stop,
                  uint64_t* count) {
  Server::Client* cl = new Server::Client(work);
  server->registerClient(cl);
  // simply work as client until stop is signalled:
  uint64_t c = 0;
  size_t perRound = ceill(1e-5 / workTime);
  uint32_t t = 0;
  while (stop->load(std::memory_order_relaxed) == 0) {
    for (size_t i = 0; i < perRound; ++i) {
      cl->inTick.store(++t, std::memory_order_relaxed);
      while (cl->outTick.load(std::memory_order_relaxed) != t) {
      }
      ++c;
    }
  }
  server->unregisterClient(cl);
  delete cl;
  *count = c;
}

int main(int argc, char* argv[]) {
  // Command line arguments:
  if (argc < 4) {
    std::cout << "Usage: servertest DIFFICULTY TESTTIME THREADS" << std::endl;
    return 0;
  }
  size_t howmuch = std::stoul(std::string(argv[1]));
  double testTime = std::stoul(std::string(argv[2]));
  int threads = std::stol(std::string(argv[3]));
  std::cout << "Difficulty: " << howmuch << std::endl;
  std::cout << "Test time : " << testTime << std::endl;
  std::cout << "Maximal number of threads: " << threads << "\n" << std::endl;

  // Work generator:
  Work work(howmuch);
  std::chrono::high_resolution_clock clock;

  // Measure a single workload:
  {
    std::cout << "Measuring a single workload..." << std::endl;
    size_t repeats = 100;
    std::chrono::duration<double> runTime;
    while (true) {
      auto startTime = clock.now();
      for (size_t i = 0; i < repeats; ++i) {
        work.dowork();
      }
      auto endTime = clock.now();
      runTime = endTime - startTime;
      if (runTime > std::chrono::duration<double>(1)) {
        break;
      }
      repeats *= 3;
    }
    workTime = runTime.count() / repeats;
    std::cout << "Work time for one unit of work: "
      << floor(workTime * 1e9) << " ns\n" << std::endl;
  }

  // Now measure how many workloads a single thread can do in a given time:
  {
    std::cout << "Running in a single thread without any locking..."
      << std::endl;
    std::atomic<int> stop(0);
    uint64_t count;
    auto startTime = clock.now();
    std::thread t(singleThread, &work, &stop, &count);
    std::this_thread::sleep_for(std::chrono::duration<double>(testTime));
    stop.store(1);
    t.join();
    auto endTime = clock.now();
    std::chrono::duration<double> runTime = endTime - startTime;
    std::cout << "  time="
      << runTime.count() << "s " << pretty(count)
      << " iterations, time per iteration: "
      << floorl(runTime.count() / static_cast<double>(count) * 1e9) << " ns"
      << "\n" << std::endl;
  }
  
  // Now measure how multiple threads fare when using a normal mutex:
  {
    std::cout << "Using multiple threads and a std::mutex..." << std::endl;
    for (int j = 1; j <= threads; ++j) {
      std::cout << "Using " << j << " threads:" << std::endl;

      std::vector<std::thread> ts;
      std::vector<uint64_t> counts;
      std::mutex mutex;
      counts.reserve(j);
      for (int i = 0; i < j; ++i) {
        counts.push_back(0);
      }
      ts.reserve(j);

      std::atomic<int> stop(0);
      auto startTime = clock.now();
      for (int i = 0; i < j; ++i) {
        ts.emplace_back(multipleThreads, &work, &mutex, &stop, &counts[i]);
      }
      std::this_thread::sleep_for(std::chrono::duration<double>(testTime));
      stop.store(1);
      for (int i = 0; i < j; ++i) {
        ts[i].join();
      }
      auto endTime = clock.now();
      std::chrono::duration<double> runTime = endTime - startTime;
      uint64_t count = 0;
      for (int i = 0; i < j; ++i) {
        count += counts[i];
      }
      std::cout << "  time="
        << runTime.count() << "s " << pretty(count)
        << " iterations, time per iteration: "
        << floorl(runTime.count() / static_cast<double>(count) * 1e9) << " ns"
        << std::endl;
      std::cout << "  thread counts:";
      for (int i = 0; i < j; ++i) {
        std::cout << " " << pretty(counts[i]);
      }
      std::cout << "\n" << std::endl;
    }
  }

  // Measure a delegating server:
  {
    std::cout << "Running in a single thread with delegation..." << std::endl;
    Server server;  // start the server thread
    for (int j = 1; j <= threads; ++j) {
      std::cout << "Using " << j << " threads:" << std::endl;
      std::atomic<int> stop(0);
      std::vector<std::thread> ts;
      std::vector<uint64_t> counts;
      counts.reserve(j);
      for (int i = 0; i < j; ++i) {
        counts.push_back(0);
      }
      ts.reserve(j);
      auto startTime = clock.now();
      for (int i = 0; i < j; ++i) {
        ts.emplace_back(clientThread, &server, &work, &stop, &counts[i]);
      }
      std::this_thread::sleep_for(std::chrono::duration<double>(testTime));
      stop.store(1);
      for (int i = 0; i < j; ++i) {
        ts[i].join();
      }
      auto endTime = clock.now();
      std::chrono::duration<double> runTime = endTime - startTime;
      uint64_t count = 0;
      for (int i = 0; i < j; ++i) {
        count += counts[i];
      }
      std::cout << "  time="
        << runTime.count() << "s " << pretty(count)
        << " iterations, time per iteration: "
        << floorl(runTime.count() / static_cast<double>(count) * 1e9) << " ns"
        << std::endl;
      std::cout << "  thread counts:";
      for (int i = 0; i < j; ++i) {
        std::cout << " " << pretty(counts[i]);
      }
      std::cout << "\n" << std::endl;
    }
  }

  // Write out dummy result to convince compiler not to optimize everything out
  {
    std::fstream dummys("/dev/null", std::ios_base::out);
    dummys << work.get() << std::endl;
  }
}
