#pragma once

#include <queue>
#include <list>
#include <vector>
#include <memory>

namespace ig{
    static constexpr std::size_t kObjectPoolDefaultSize = 10000;      // 对象池默认缓存对象的数量
    static constexpr std::size_t kObjectPoolDefaultExtendSize = 1000; //
    
    template<typename ObjectType>
    struct ObjectPoolDefaultInitializer{ // 仿函数模板
        void operator()(ObjectType* object) const{}
    };

    template<typename ObjectType,std::size_t N = kObjectPoolDefaultSize,
        typename Initializer = ObjectPoolDefaultInitializer<ObjectType>>
    class sharedObjectPool{
    public:
        [[nodiscard]] static sharedObjectPool& getInstance(){
            static sharedObjectPool pool(N);  // 单例模式构造
            return pool;
        }

        [[nodiscard]] std::shared_ptr<ObjectType> Get(){
            ObjectType* ptr = nullptr;
            if (queue_.empty()) // 如果空闲队列为空，则申请拓展多个对象
            {
                Extend(kObjectPoolDefaultExtendSize);
            }
            // 若空闲队列不为空，则从队列中获取对象即可
            ptr = queue_.front();
            queue_.pop();
            kInitializer(ptr); // 重置对象状态（清空数据），设置默认值，准备对象以供使用
            
            // 返回shared_ptr接口
            return std::shared_ptr<ObjectType>(ptr,[&](ObjectType* p){
                queue_.push(p); // 当对象引用计数为0时，将对象视作空闲对象返回对象池中
            });
        }

        size_t getCapacity() const {
            return capacity_;
        }
        
        void setCapacity(size_t num){
            if(capacity_ < num){
                Extend(capacity_ - num);
            }
        }
        


    private:
        std::size_t capacity_ = 0;
        std::queue<ObjectType*> queue_;  // 空闲对象队列 
        ObjectType* cache_ = nullptr;
        std::list<ObjectType*> extend_cache_;
        const std::size_t kDefaultCacheSize_; // 默认缓存对象的数量

        inline static const Initializer kInitializer = Initializer(); // 初始化器，重置对象状态（清空数据），设置默认值，准备对象以供使用

        explicit sharedObjectPool(const std::size_t pool_size) : 
            kDefaultCacheSize_(pool_size)
        {   
            cache_ = new ObjectType[kDefaultCacheSize_];
            for(size_t i = 0; i < kDefaultCacheSize_;i++){
                queue_.push(&cache_[i]);      // 在构造池子时，将缓存对象加入空闲队列
                kInitializer(&cache_[i]);    // 重置对象状态（清空数据），设置默认值，准备对象以供使用
            }
            capacity_ = kDefaultCacheSize_;      // 将对象全部丢进空闲队列后，设置池容量
        }

        ~sharedObjectPool(){
            if(cache_){
                delete[] cache_;
                cache_ = nullptr;
            }

            for(auto& ptr : extend_cache_){
                delete ptr;
            }
            extend_cache_.clear();
        }

        void Extend(const size_t num){
            ObjectType* ptr = new ObjectType[num];
            for (size_t i = 0; i < num;++i){
                
                extend_cache_.push_back(&ptr[i]);
                queue_.push(&ptr[i]);
                kInitializer(&ptr[i]);  // 重置对象状态（清空数据），设置默认值，准备对象以供使用
            }
            capacity_ += num; 
        }
    };
}