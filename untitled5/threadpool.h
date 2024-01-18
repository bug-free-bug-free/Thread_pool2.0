#ifndef untitled5_THREADPOOL_H
#define untitled5_THREADPOOL_H
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <any>
#include <semaphore>
#include <thread>
#include <future>
const int MAX_SIZE=1024;
const int THREAD_MAX_THRESHHOLD=10;
const int THREAD_MAX_IDLE_TIME=60;  //单位：秒

enum PoolMode{  //线程池支持的模式
    MODE_FIXED,
    MODE_CACHED
};

class Thread {  //线程类型
public:
    using ThreadFunc=std::function<void(int)>; //线程函数对象类型
    Thread(ThreadFunc func)
    :func_(func),threadId_(generateId_++)
    {}
    ~Thread()=default;
    void start()
    {
        std::thread t(func_,threadId_);
        t.detach();
    }
    int getId()const
    {
        return threadId_;
    }
private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_;  //保存线程id
};

int Thread::generateId_=0;

class ThreadPool {  //线程池类
public:
    ThreadPool()
    :initThreadSize_(0),
    taskSize_(0),
    taskQueMaxThreshHold_(MAX_SIZE),
    poolMode_(PoolMode::MODE_FIXED),
    isPoolRunning_(false),
    idleThreadSize_(0),
    threadSizeThreshHold_(THREAD_MAX_THRESHHOLD),
    curThreadSize_(0)
    {}
    ~ThreadPool()
    {
        isPoolRunning_= false;
        notEmpty_.notify_all();
        //等待所有线程池线程返回
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        exitCond_.wait(lock,[&](){
            return threads_.size()==0;
        });
    }

    bool checkRunningState()const
    {
        return isPoolRunning_;
    }

    void start(int size=std::thread::hardware_concurrency())     //开始创建线程
    {
        //设置线程池的运行状态
        isPoolRunning_=true;
        //记录线程个数
        initThreadSize_=size;
        curThreadSize_=size;
        for(int i=0;i<initThreadSize_;i++)
        {
            auto p=std::make_unique<Thread>(
                    std::bind(&ThreadPool::threadFunc,this,std::placeholders::_1));
            int threadId=p->getId();
            threads_.emplace(threadId,std::move(p));
        }
        for(int i=0;i<initThreadSize_;i++)
        {
            threads_[i]->start();   //分别启动去执行一个线程函数
            idleThreadSize_++;  //记录初始空闲线程的数量
        }
    }

    void setMode(PoolMode mode)//设置模式
    {
        if(checkRunningState())
        {
            return;
        }
        poolMode_=mode;
    }
    void setTaskQueMaxThreshHold(int threshhold)
    {  //设置任务队列数量最大值
        if(checkRunningState())
        {
            return;
        }
        taskQueMaxThreshHold_=threshhold;
    }
    void setThreadSizeThreshHold(int threshhold=std::thread::hardware_concurrency()*2)
    {
        if(checkRunningState())
        {
            return;
        }
        if(poolMode_==PoolMode::MODE_CACHED)
        {
            threadSizeThreshHold_ = threshhold;
        }
    }

    //给线程池提交任务
    //使用可变模板编程，让submitTask可以接受任意函数和任意数量的参数
    //pool.submitTask(sum1,10,20);
    //返回值future<>
    template<class Func,typename... Args>
    auto submitTask(Func&& func,Args&&... args)-> std::future<decltype(func(args...))>
    {
        //打包任务，放入任务队列里面
        using RType= decltype(func(args...));
        auto task=std::make_shared<std::packaged_task<RType()>>(
        std::bind(std::forward<Func>(func),std::forward<Args>(args)...));

        std::future<RType> result=task->get_future();
        //获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        //用户提交任务，如果等待时间超过了1s，则显示提交失败，返回
        if(!notFull_.wait_for(lock,std::chrono::seconds(1),[&]
        {
            if(taskQue_.size()==taskQueMaxThreshHold_)
                return false;
            return true;
        }))
        {
            std::cout<<"任务提交失败"<<std::endl;
            auto task=std::make_shared<std::packaged_task<RType()>>(
            [&]()->RType{return RType();});
            (*task)();
            return task->get_future();
        }
        //将任务放进任务队列中
        //taskQue_.emplace(sp);
        //using Task=std::function<void()>;
        taskQue_.emplace([task]() {(*task)();});
        taskSize_++;
        //通过notEmpty进行通知
        notEmpty_.notify_all();
        //如果是cached模式，任务比较紧急，
        if(poolMode_==PoolMode::MODE_CACHED
           &&taskSize_>idleThreadSize_
           &&curThreadSize_<threadSizeThreshHold_)
        {
            //创建新线程
            auto p=std::make_unique<Thread>(std::bind(
                    &ThreadPool::threadFunc,this,std::placeholders::_1));
            int threadId=p->getId();
            threads_.emplace(threadId,std::move(p));
            curThreadSize_++;
            //启动
            threads_[threadId]->start();
            //修改线程相关变量
            curThreadSize_++;
            idleThreadSize_++;
        }
        //返回人物的Result对象
        return result;
    }

    ThreadPool(const ThreadPool& t)=delete;
    void operator=(const ThreadPool& t)=delete;

    void threadFunc(int threadid)
    {
        auto lastTime=std::chrono::high_resolution_clock().now();
        //必须等所有任务都执行完，线程才能结束
        while(true)
        {
            //获取锁
            std::cout<<std::this_thread::get_id()<<"开始抢任务"<<std::endl;
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            while(taskQue_.size()==0)
            {
                //线程池要结束，回收资源
                if(!isPoolRunning_)
                {
                    threads_.erase(threadid);
                    std::cout<<"threadid:"<<std::this_thread::get_id()<<"exit"<<std::endl;
                    exitCond_.notify_all();
                    return;
                }
                if(poolMode_==PoolMode::MODE_CACHED)
                {
                    //条件变量，超时返回了
                    //每一秒返回一次
                    //cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程回收掉
                    //回收超过initThreadSize_数量的线程
                    //当前时间-上一次执行的时间
                    //线程通信
                    if(std::cv_status::timeout==notEmpty_.wait_for(lock,std::chrono::seconds(1)))
                    {
                        auto now=std::chrono::high_resolution_clock ().now();
                        auto dur=std::chrono::duration_cast<std::chrono::seconds>(now-lastTime);
                        if(dur.count()>=THREAD_MAX_IDLE_TIME&&curThreadSize_>initThreadSize_)
                        {
                            //开始回收当前线程
                            //记录线程数量的相关的值要修改
                            //把线程对象从线程列表容器中删除   （很难）
                            threads_.erase(threadid);
                            curThreadSize_--;
                            idleThreadSize_--;
                            std::cout<<"threadid:"<<std::this_thread::get_id()<<"exit"<<std::endl;
                            return;
                        }
                    }
                }
                else
                {
                    notEmpty_.wait(lock);
                }
            }

            idleThreadSize_--;  //空闲线程数量减一
            //线程去任务队列中获取任务
            auto task=taskQue_.front();
            taskQue_.pop();
            taskSize_--;
            lock.unlock();
            //通过notFull进行线程通信，“昭告天下”
            notFull_.notify_all();
            //任务执行

            task(); //执行function<void()>
            idleThreadSize_++;  //空闲线程数量加一
            lastTime=std::chrono::high_resolution_clock().now();    //更新
            std::cout<<std::this_thread::get_id()<<"任务执行成功"<<std::endl;
        }
    }
private:
    std::unordered_map<int,std::unique_ptr<Thread>> threads_;   //线程列表
    int  initThreadSize_;       //初始的线程数量
    int threadSizeThreshHold_;  //线程数量上限
    std::atomic_int curThreadSize_; //记录当前线程池中线程的总数量
    std::atomic_int idleThreadSize_;    //记录空闲线程的数量

    //Task任务=》函数对象
    using Task=std::function<void()>;
    std::queue<Task> taskQue_;     //任务队列
    std::atomic_int taskSize_;      //任务数量
    int taskQueMaxThreshHold_;      //任务队列数量最大值

    std::mutex taskQueMtx_;     //保证任务队列线程安全
    std::condition_variable notFull_;       //任务队列不满
    std::condition_variable exitCond_;  //等到线程资源全部回收
    std::condition_variable notEmpty_;      //任务队列不空
    PoolMode poolMode_;      //当前线程池的工作模式
    std::atomic_bool isPoolRunning_  ;//表示当前线程池的启动状态

};


#endif //untitled5_THREADPOOL_H
