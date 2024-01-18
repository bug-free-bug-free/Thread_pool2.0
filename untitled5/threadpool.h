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
const int THREAD_MAX_IDLE_TIME=60;  //��λ����

enum PoolMode{  //�̳߳�֧�ֵ�ģʽ
    MODE_FIXED,
    MODE_CACHED
};

class Thread {  //�߳�����
public:
    using ThreadFunc=std::function<void(int)>; //�̺߳�����������
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
    int threadId_;  //�����߳�id
};

int Thread::generateId_=0;

class ThreadPool {  //�̳߳���
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
        //�ȴ������̳߳��̷߳���
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        exitCond_.wait(lock,[&](){
            return threads_.size()==0;
        });
    }

    bool checkRunningState()const
    {
        return isPoolRunning_;
    }

    void start(int size=std::thread::hardware_concurrency())     //��ʼ�����߳�
    {
        //�����̳߳ص�����״̬
        isPoolRunning_=true;
        //��¼�̸߳���
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
            threads_[i]->start();   //�ֱ�����ȥִ��һ���̺߳���
            idleThreadSize_++;  //��¼��ʼ�����̵߳�����
        }
    }

    void setMode(PoolMode mode)//����ģʽ
    {
        if(checkRunningState())
        {
            return;
        }
        poolMode_=mode;
    }
    void setTaskQueMaxThreshHold(int threshhold)
    {  //������������������ֵ
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

    //���̳߳��ύ����
    //ʹ�ÿɱ�ģ���̣���submitTask���Խ������⺯�������������Ĳ���
    //pool.submitTask(sum1,10,20);
    //����ֵfuture<>
    template<class Func,typename... Args>
    auto submitTask(Func&& func,Args&&... args)-> std::future<decltype(func(args...))>
    {
        //������񣬷��������������
        using RType= decltype(func(args...));
        auto task=std::make_shared<std::packaged_task<RType()>>(
        std::bind(std::forward<Func>(func),std::forward<Args>(args)...));

        std::future<RType> result=task->get_future();
        //��ȡ��
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        //�û��ύ��������ȴ�ʱ�䳬����1s������ʾ�ύʧ�ܣ�����
        if(!notFull_.wait_for(lock,std::chrono::seconds(1),[&]
        {
            if(taskQue_.size()==taskQueMaxThreshHold_)
                return false;
            return true;
        }))
        {
            std::cout<<"�����ύʧ��"<<std::endl;
            auto task=std::make_shared<std::packaged_task<RType()>>(
            [&]()->RType{return RType();});
            (*task)();
            return task->get_future();
        }
        //������Ž����������
        //taskQue_.emplace(sp);
        //using Task=std::function<void()>;
        taskQue_.emplace([task]() {(*task)();});
        taskSize_++;
        //ͨ��notEmpty����֪ͨ
        notEmpty_.notify_all();
        //�����cachedģʽ������ȽϽ�����
        if(poolMode_==PoolMode::MODE_CACHED
           &&taskSize_>idleThreadSize_
           &&curThreadSize_<threadSizeThreshHold_)
        {
            //�������߳�
            auto p=std::make_unique<Thread>(std::bind(
                    &ThreadPool::threadFunc,this,std::placeholders::_1));
            int threadId=p->getId();
            threads_.emplace(threadId,std::move(p));
            curThreadSize_++;
            //����
            threads_[threadId]->start();
            //�޸��߳���ر���
            curThreadSize_++;
            idleThreadSize_++;
        }
        //���������Result����
        return result;
    }

    ThreadPool(const ThreadPool& t)=delete;
    void operator=(const ThreadPool& t)=delete;

    void threadFunc(int threadid)
    {
        auto lastTime=std::chrono::high_resolution_clock().now();
        //�������������ִ���꣬�̲߳��ܽ���
        while(true)
        {
            //��ȡ��
            std::cout<<std::this_thread::get_id()<<"��ʼ������"<<std::endl;
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            while(taskQue_.size()==0)
            {
                //�̳߳�Ҫ������������Դ
                if(!isPoolRunning_)
                {
                    threads_.erase(threadid);
                    std::cout<<"threadid:"<<std::this_thread::get_id()<<"exit"<<std::endl;
                    exitCond_.notify_all();
                    return;
                }
                if(poolMode_==PoolMode::MODE_CACHED)
                {
                    //������������ʱ������
                    //ÿһ�뷵��һ��
                    //cachedģʽ�£��п����Ѿ������˺ܶ���̣߳����ǿ���ʱ�䳬��60s��Ӧ�ðѶ�����̻߳��յ�
                    //���ճ���initThreadSize_�������߳�
                    //��ǰʱ��-��һ��ִ�е�ʱ��
                    //�߳�ͨ��
                    if(std::cv_status::timeout==notEmpty_.wait_for(lock,std::chrono::seconds(1)))
                    {
                        auto now=std::chrono::high_resolution_clock ().now();
                        auto dur=std::chrono::duration_cast<std::chrono::seconds>(now-lastTime);
                        if(dur.count()>=THREAD_MAX_IDLE_TIME&&curThreadSize_>initThreadSize_)
                        {
                            //��ʼ���յ�ǰ�߳�
                            //��¼�߳���������ص�ֵҪ�޸�
                            //���̶߳�����߳��б�������ɾ��   �����ѣ�
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

            idleThreadSize_--;  //�����߳�������һ
            //�߳�ȥ��������л�ȡ����
            auto task=taskQue_.front();
            taskQue_.pop();
            taskSize_--;
            lock.unlock();
            //ͨ��notFull�����߳�ͨ�ţ����Ѹ����¡�
            notFull_.notify_all();
            //����ִ��

            task(); //ִ��function<void()>
            idleThreadSize_++;  //�����߳�������һ
            lastTime=std::chrono::high_resolution_clock().now();    //����
            std::cout<<std::this_thread::get_id()<<"����ִ�гɹ�"<<std::endl;
        }
    }
private:
    std::unordered_map<int,std::unique_ptr<Thread>> threads_;   //�߳��б�
    int  initThreadSize_;       //��ʼ���߳�����
    int threadSizeThreshHold_;  //�߳���������
    std::atomic_int curThreadSize_; //��¼��ǰ�̳߳����̵߳�������
    std::atomic_int idleThreadSize_;    //��¼�����̵߳�����

    //Task����=����������
    using Task=std::function<void()>;
    std::queue<Task> taskQue_;     //�������
    std::atomic_int taskSize_;      //��������
    int taskQueMaxThreshHold_;      //��������������ֵ

    std::mutex taskQueMtx_;     //��֤��������̰߳�ȫ
    std::condition_variable notFull_;       //������в���
    std::condition_variable exitCond_;  //�ȵ��߳���Դȫ������
    std::condition_variable notEmpty_;      //������в���
    PoolMode poolMode_;      //��ǰ�̳߳صĹ���ģʽ
    std::atomic_bool isPoolRunning_  ;//��ʾ��ǰ�̳߳ص�����״̬

};


#endif //untitled5_THREADPOOL_H
