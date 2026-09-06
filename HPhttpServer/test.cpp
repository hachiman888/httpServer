#include <iostream>
#include "shared_object_pool.hpp"
#include <chrono>


struct TestObj{
    int a;
    double b;
    char c[4096];

    TestObj(int A,int B):a(A),b(B) {
        c[0] = '\0';
    }
};

void stdTest(){
    auto start = std::chrono::high_resolution_clock::now();
    for(int i = 0; i < 100000;i++){
        TestObj* p = new TestObj(1,2);
        [[maybe_unused]] volatile auto sink = p;
    }
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();
    std::cout << "std test : " << duration << std::endl;
}

void poolTest(){
    auto& pool = ig::sharedObjectPool<TestObj>::getInstance();
    auto start = std::chrono::high_resolution_clock::now(); 
    for(int i = 0; i < 100000;i++){
        [[maybe_unused]] volatile auto sink = pool.Get(1,2).get();
    }
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();
    std::cout << "pool test : " << duration << std::endl;
}

int main(){
    stdTest();
    poolTest();
}
