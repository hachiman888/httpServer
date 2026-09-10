#pragma once

#include <cstddef>
#include <queue>
#include <list>
#include <vector>
#include <memory>
#include <utility>
#include <mutex>
#include <type_traits>
#include <print>
#include <thread>


/*
主要改进与 Bug 修复说明
解决对默认构造函数的依赖（核心需求）

原问题：采用 new ObjectType[N] 批量开辟内存，强行要求类型具有默认无参构造函数。

重构方案：采用 ::operator new[] 申请裸内存块（不触发构造），并利用 Get(Args&&... args) 完美支持完美转发（Variadic Templates + Perfect Forwarding）。只有在使用者调用 .Get(...) 时，才会触发 Placement New (::new (raw_ptr) ObjectType(...)) 进行初始化。

修复 Extend 减法溢出 Bug

原代码：Extend(capacity_ - num)。当 capacity_ < num 时，计算结果下溢为一个极其庞大的无符号正整数，导致试图分配太大的内存而触发 Crash。

修复后：修正为 Extend(num - capacity_)。

修复内存释放未定义行为（Heap Corruption）

原代码：Extend 中用 new[] 分配数组，但在析构函数中对数组内每一个元素地址遍历调用标量 delete ptr，导致指针类型错配与重复释放内存崩溃。

修复后：extend_blocks_ 专门保存每次扩容申请的起始块首地址，析构时统一使用 ::operator delete[] 安全释放整块内存。

析构与生命周期管理精准化

在 shared_ptr 的自定义 Deleter 中，当对象使用完毕归还时：手动显式调用 p->~ObjectType() 触发析构函数清理对象状态（释放内部资源），并将内存块地址丢回 free_queue_，彻底避免了数据残留问题。

线程安全强化（Thread Safety）

增加了 std::mutex 互斥锁，确保在多线程高并发场景下从 pool 中申请和归还内存时不发生竞争条件（Data Race）。
*/

namespace ig{
    static constexpr std::size_t kObjectPoolDefaultSize = 1500;      // 对象池默认缓存对象的数量
    static constexpr std::size_t kObjectPoolDefaultExtendSize = 300; //
    

    template<typename ObjectType,std::size_t N = kObjectPoolDefaultSize>
    class sharedObjectPool{
    public:
        [[nodiscard]] static inline sharedObjectPool& getInstance(){
            static sharedObjectPool pool(N);  // 单例模式构造
            return pool;
        }

        sharedObjectPool(const sharedObjectPool&) = delete;
        sharedObjectPool&(operator=(const sharedObjectPool&)) = delete;
        sharedObjectPool(const sharedObjectPool&&) = delete;
        sharedObjectPool&(operator=(const sharedObjectPool&&)) = delete;

        /**
        * @brief 从对象池获取一个对象（支持任意构造函数参数）
        * @tparam Args 构造函数参数类型
        * @param args 传递给 ObjectType 构造函数的参数
        */

        template<typename... Args>
        [[nodiscard]] std::shared_ptr<ObjectType> Get(Args&&... args){
            std::lock_guard<std::mutex> lock(mutex_);

            if(free_queue_.empty()){
                // 若空闲队列为空，则需要拓展对象池
                Extend(kObjectPoolDefaultExtendSize);
                //std::println("called sharedObjectPool's extend...,current queue size:{}",free_queue_.size());
            }

            // 1.从空闲队列中取出一块未初始化的裸内存
            void* raw_ptr = free_queue_.front();
            free_queue_.pop();

            // 2. 在裸内存上使用placement new进行构造
            // 使用完美转发保证参数属性
            ObjectType* obj_ptr = ::new(raw_ptr) ObjectType(std::forward<Args>(args)...);
            //std::println("called sharedObjectPool's get...,current queue size:{}",free_queue_.size());

            // 3. 返回自定义deleter 的 shared_ptr
            // 当引用计数归零时，手动调用对象的析构函数，并将空闲内存变成裸内存返回池中
            return std::shared_ptr<ObjectType>(obj_ptr,[this](ObjectType* p){
                if(p){
                    p->~ObjectType(); // 手动调用析构函数，清理内部资源，但保留内存
                    std::lock_guard<std::mutex> lock(this->mutex_);
                    this->free_queue_.push(static_cast<void*>(p)); // 归还内存块
                }
            });
        }

        size_t getCapacity() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return capacity_;
        }

        size_t getFreeCount() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return free_queue_.size();
        }
        
        void setCapacity(size_t num){
            std::lock_guard<std::mutex> lock(mutex_);
            if(capacity_ < num){
                Extend(num - capacity_);
            }
        }

        ~sharedObjectPool(){
            std::lock_guard<std::mutex> lock(mutex_);
            // 释放预分配的主内存块
            if(primary_block_){
                ::operator delete[](primary_block_);
                primary_block_ = nullptr;
            }

            // 按连续内存块精准释放扩展内存
            for(void* block : extend_blocks_){
                ::operator delete[](block);
            }
            extend_blocks_.clear();
            // free_queue_ 会在其自身析构时自动清空内部的 void* 成员（不需要也不应该 delete 它们）
            //std::println("sharedObjectPool destructed...");
        }
        


    private:
        std::size_t capacity_ = 0;
        std::queue<void*> free_queue_;             // 存放可用于放置对象的裸内存地址
        void* primary_block_ = nullptr;            // 主内存块指针
        std::vector<void*> extend_blocks_;          // 扩容块指针列表（用于统一析构释放）
        mutable std::mutex mutex_;                 // 保证多线程并发 Get / Return 的安全


        explicit sharedObjectPool(const std::size_t pool_size)
        {   
            if (pool_size > 0)
            {   
                // 使用 ::operator delete[] / ::operator new[] 直接申请裸内存（不调用构造函数）
                // ::operator new 也只分配内存，不构造对象
                // 获取连续的复数个ObjectType大小的块，并且内存对齐
                // 在 C++ 标准中，sizeof(T) 已经包含了编译器为了满足对齐要求而自动添加的尾部填充字节（Trailing Padding）。
                // 所以不会发生错位
                primary_block_ = ::operator new[](sizeof(ObjectType) * pool_size, 
                    std::align_val_t{alignof(ObjectType)});

                // 使用char* 指针指向裸内存的首字节,因为char大小为1字节，天然适合处理无符号字节流,其他的类型不一定合适
                char* byte_ptr = static_cast<char*>(primary_block_);
                for(std::size_t i = 0; i < pool_size; i++){
                    free_queue_.push(static_cast<void*>(byte_ptr + i * sizeof(ObjectType))); 
                }
                capacity_ = pool_size;
            }
        }

        void Extend(const size_t num){
            if(num == 0) return;

            //  动态分配一块能容纳num个ObjectType的原始内存块
            void* new_block = ::operator new[](sizeof(ObjectType) * num,
            std::align_val_t{alignof(ObjectType)}); // C++17,std::align_val_t
            extend_blocks_.push_back(new_block);

            char* byte_ptr = static_cast<char*>(new_block);
            for(size_t i = 0; i < num;i++){
                free_queue_.push(static_cast<void*>(byte_ptr + i * sizeof(ObjectType)));
            }
            capacity_ += num;
        }
    };
} // namespace ig