#pragma once

#include <__concepts/derived_from.h>
#include <__threading_support>
#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/_types/_int8_t.h>
#include <sys/_types/_ssize_t.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <bitset>
#include <iostream>

namespace Concurrent {
    constexpr size_t MAX_THREADS_AMNT = 1;

    namespace detail {
        using HPCell = void*;
    } // namespace detail

    class HazardPtr {
        detail::HPCell* ThreadLocalCellAddr = nullptr;
        std::function<void(detail::HPCell)> Deleter = {};

    public:
        HazardPtr() = delete;
        HazardPtr(detail::HPCell* threadLocalCellAddr, std::function<void(detail::HPCell)> deleter) : 
                            ThreadLocalCellAddr(threadLocalCellAddr), Deleter(std::move(deleter)) {}
        HazardPtr(HazardPtr&& hp) : ThreadLocalCellAddr(hp.ThreadLocalCellAddr) {
            hp.ThreadLocalCellAddr = nullptr;
        }

        bool Empty() const {
            return *ThreadLocalCellAddr == nullptr;
        }

        template <typename T>
        T* Protect(const std::atomic<T*>& varToProtect) {
            T* ptr;
            do {
                *ThreadLocalCellAddr = ptr = varToProtect.load();
            } while (ptr != varToProtect.load());
            return ptr;
        }

        void Retire();
        ~HazardPtr() {
            // No need to sync, because other thread can write something from cell if and only if it has nullptr value
            *ThreadLocalCellAddr = nullptr;
        }
    };

    class HazardPtrManager;

    namespace detail {
        struct IHazardPtrsStorage {
            static constexpr std::size_t Size = 5;
            thread_local static inline std::vector<std::pair<detail::HPCell, std::function<void(HPCell)>>> RetireStorage;

            // Return new HazardPtr
            virtual HazardPtr Create(std::__thread_id, std::function<void(HPCell)>) = 0;
            virtual void Retire(detail::HPCell*, const std::function<void(HPCell)>&) = 0;

            virtual ~IHazardPtrsStorage() {};
        };

        struct MapHazardPtrsStorage : IHazardPtrsStorage {
            std::unordered_map<std::__thread_id, std::array<detail::HPCell, Size>> HPStorage;

            // Traverse all HP thread local storage to find unprotected values from curr RetireStorage
            void ReclaimUnprotected() {
                for (auto& [reclaimCandidate, deleter] : RetireStorage) {
                    // Use lamda for "break" from nested loop
                    [this, reclaim = deleter, reclaimCandidate = reclaimCandidate]() {
                        for (auto& [threadId, threadStorage] : HPStorage) {
                            for (auto protectedAddr : threadStorage) {
                                if (protectedAddr == reclaimCandidate) {
                                    return;
                                }
                            }
                        }
                        // Reclaim unprotected addr
                        reclaim(reclaimCandidate);
                    }();
                }
            }

        public:
            HazardPtr Create(std::__thread_id id, std::function<void(HPCell)> deleter) override {
                auto& threadLocalHazardPtrs = HPStorage[id];
                auto freePtr = std::find(threadLocalHazardPtrs.begin(), threadLocalHazardPtrs.end(), nullptr);
                if (freePtr == threadLocalHazardPtrs.end()) {
                    // TODO: Mb clear retain arr here
                    throw std::runtime_error("Thread local storage for hazard ptr overload");
                } else {
                    return HazardPtr(freePtr, std::move(deleter));
                }
            }

            void Retire(detail::HPCell* addr, const std::function<void(void*)>& reclaim) override {
                HPCell oldValue = *addr;
                *addr = nullptr;
                if (RetireStorage.size() == Size * MAX_THREADS_AMNT + 1) {
                    ReclaimUnprotected();
                }
                RetireStorage.push_back({oldValue, reclaim});
            }

            ~MapHazardPtrsStorage() override = default;
        };
    } // namespace detail

    class HazardPtrManager {
        HazardPtrManager() = default;
        
    public:
        static HazardPtrManager& GetInstance() {
            static HazardPtrManager manager;
            return manager;
        }

        // Replace storage with storage of new type and release ownership of old one
        template <typename T>
            requires std::is_base_of_v<detail::IHazardPtrsStorage, T> && (requires { T::RetireStorage; })
        auto SetStorageType() {
            // We need to keep max (size of retire arr) > (size of Hp) * (threads amnt) for proper work of clearing algo
            T::RetireStorage.reserve(T::Size * MAX_THREADS_AMNT + 1);
            return std::exchange(HazardStorage, std::make_unique<T>());
        }

        HazardPtr Create(std::function<void(detail::HPCell)> deleter = {}) {
            std::lock_guard lock(M);
            return HazardStorage->Create(std::this_thread::get_id(), deleter);
        }

        void Reclaim(detail::HPCell* addr, const std::function<void(detail::HPCell)>& reclaim) {
            std::lock_guard lock(M);
            return HazardStorage->Retire(addr, reclaim);
        }

    private:
        std::mutex M;
        std::unique_ptr<detail::IHazardPtrsStorage> HazardStorage;
    };

    inline void HazardPtr::Retire() {
        HazardPtrManager::GetInstance().Reclaim(ThreadLocalCellAddr, Deleter);
    }

    inline HazardPtr MakeHazardPtr() {
        return HazardPtrManager::GetInstance().Create();
    }

} // namespace Concurrent
