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
    std::atomic<int> what;  // starts as 0, which means nothing to do
                            // a positive number indicates that the client
                            // wants a job to be done, when done, the server
                            // sets it to the negative of the job id
    Work* work;
    Client(Work* w) : what(0), work(w) {
    }
  };

 private:
  std::mutex mutex;
  size_t maxNrClients;
  Client** clients;
  std::atomic<size_t> nrClients;
  bool started;
  std::thread server;

 public:
  Server(size_t m) 
    : maxNrClients(m), clients(new Client*[m]), nrClients(0), started(false),
      server(&Server::run, this) {
  }

  ~Server() {
    if (nrClients > 0) {
      std::cout << "Warning: Server has clients on destruction!" << std::endl;
    }
    server.join();
  }

  bool registerClient(Client* c) {
    std::unique_lock<std::mutex> guard(mutex);
    if (nrClients >= maxNrClients) {
      return false;
    }
    clients[nrClients] = c;
    ++nrClients;
    started = true;
    return true;
  }

 private:

  void unregisterClient(size_t pos) {
    std::unique_lock<std::mutex> guard(mutex);
    clients[pos] = clients[nrClients-1];
    --nrClients;
  }

  void run() {
    while (true) {
      size_t nr = nrClients;
      if (nr == 0) {
        if (started) {
          return;
        }
        //std::this_thread::sleep_for(std::chrono::duration<double>(0.001));
      } else {
        for (size_t i = 0; i < nr; ++i) {
          int what = clients[i]->what.load(std::memory_order_acquire);
          if (what > 0) {
            switch (what) {
              case 1:
                unregisterClient(i);
                --nr;
                break;
              case 2:
                clients[i]->work->dowork();
                break;
            }
            clients[i]->what.store(-what, std::memory_order_release);
          }
        }
      }
    }
  }
};
 
void clientThread(Server* server, Work* work, std::atomic<int>* stop,
                  uint64_t* count) {
  Server::Client cl(work);
  server->registerClient(&cl);
  // simply work as client until stop is signalled:
  uint64_t c = 0;
  size_t perRound = ceill(1e-5 / workTime);
  while (stop->load() == 0) {
    for (size_t i = 0; i < perRound; ++i) {
      cl.what = 2;
      while (cl.what.load(std::memory_order_acquire) != -2) {
      }
      ++c;
    }
  }
  *count = c;
  cl.what = 1;
  while (cl.what.load(std::memory_order_acquire) != -1) {
  }
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
    Server server(threads);  // start the server thread
    std::atomic<int> stop(0);
    uint64_t count;
    auto startTime = clock.now();
    std::thread t(clientThread, &server, &work, &stop, &count);
    std::this_thread::sleep_for(std::chrono::duration<double>(testTime));
    stop.store(1);
    t.join();
    auto endTime = clock.now();
    std::chrono::duration<double> runTime = endTime - startTime;
    std::cout << "  time="
      << runTime.count() << "s " << pretty(count)
      << " iterations, time per iteration: "
      << floorl(runTime.count() / static_cast<double>(count) * 1e9) << " ns"
      << std::endl;
    
  }

  // Write out dummy result to convince compiler not to optimize everything out
  {
    std::fstream dummys("/dev/null", std::ios_base::out);
    dummys << work.get() << std::endl;
  }
}
