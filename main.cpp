#include "hazard-ptr.hpp"
#include <__chrono/duration.h>
#include <cstddef>
#include <thread>
#include <vector>
#include <iostream>

struct DeathScreamer {
    DeathScreamer() = default;
    ~DeathScreamer() {
        std::cout << "Deleted" << std::endl;
    }
};

int main() {
    auto& manager = Concurrent::HazardPtrManager::GetInstance();
    manager.SetStorageType<Concurrent::detail::MapHazardPtrsStorage>();
    {
        DeathScreamer var;
        std::atomic<DeathScreamer*> atomicPtr{&var};

        using namespace std::chrono_literals;

        auto thread1= std::thread([&](){
            auto hp = manager.Create();
            hp.Protect(atomicPtr);
            std::this_thread::sleep_for(5s);
        });
        std::this_thread::sleep_for(1s);

        auto thread2 = std::thread([&]() {
            auto hp = manager.Create();
            hp.Protect(atomicPtr);
        });
        auto thread3 = std::thread([&]() {
            auto hp = manager.Create([](void * arg) {
                std::cout << "Deleter is called" << std::endl;
                delete (DeathScreamer *) arg;
            });

            hp.Protect(atomicPtr);
            for (int i = 0; i < 10; ++i) {
                std::cout << "hp.empty() = " << hp.Empty() << std::endl;
                hp.Retire();
            }
        });
        thread2.join();
        thread3.join();
        std::cout << "Second and third threads finished" << std::endl;
        thread1.join();
    }

    return 0;
}