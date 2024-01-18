#include <iostream>
#include "threadpool.h"

int sum(int a,int b,int c)
{
    return a+b+c;
}

int main()
{
    ThreadPool pool;
    pool.start(4);
    std::future<int> f=pool.submitTask(sum,1,2,3);
    std::cout<<f.get()<<std::endl;
    return 0;
}
