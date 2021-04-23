#include <iostream>
#include <mutex>
#include <pthread.h>
#include <unistd.h>

using namespace std;
/*任务队列结构体*/
typedef struct task
{
    void * (*func)(void*);   //回调函数
    void * arg;              //回调函数的参数
}threadpool_task_t;
//线程池
struct threadPool_t
{
    pthread_mutex_t lock;
    pthread_mutex_t thread_counter;  //记录忙线程个数的锁 busy_thr_num

    pthread_cond_t queue_not_full;  // 任务队列不为满条件变量，当任务队列为满时，不满足条件变量，线程就会阻塞
    pthread_cond_t queue_not_empyt; //任务队列不为空时条件变量，当认为队列为空时，则线程阻塞在这个变量，等待任务

    pthread_t * threads;   // 指向线程数组
    pthread_t adjust_tid;   //管理线程
    threadpool_task_t * task_queue;

    int min_thr_num;
    int max_thr_num;
    int live_thr_num;  //当前存活的线程数
    int busy_thr_num;  //当前忙状态线程数
    int wait_exit_thr_num; //要销毁的线程数

    /*任务队列*/
    int queue_front;
    int queue_rear;
    int queue_size;    // 当前任务队列的任务数
    int queue_max_size;  // 当前任务队列的容量

    int shutdown;  //状态标志位
};

threadPool_t *threadpool_create(int min_thr_num,int max_thr_num,int queue_max_size);
int threadpool_add(threadPool_t * pool,void* (func)(void*),void * arg);
void * threadpool_thread(void*threadpool);

void * adjust_thread(void*threadpool);
int threadpool_destory(threadPool_t*pool);
int threadpool_free(threadPool_t*pool);


                     /*线程的创建*/
threadPool_t *threadpool_create(int min_thr_num,int max_thr_num,int queue_max_size)
{
    int i;
    threadPool_t * pool = nullptr;

    do
    {
    if((pool = static_cast<threadPool_t*>(malloc(sizeof(threadPool_t))))==nullptr)
    {
        cout<<"pool malloc failed"<<endl;
        break;
    }

    pool->min_thr_num = min_thr_num;    //线程数最小值
    pool->max_thr_num = max_thr_num;      //线程数最大值
    pool->queue_max_size = queue_max_size; // 任务队列的最大值

    pool->queue_size = 0;
    pool->queue_front = 0;
    pool->queue_rear = 0;
    pool->live_thr_num = min_thr_num;
    pool->busy_thr_num = 0;
    pool->wait_exit_thr_num = 0;
    pool->shutdown = 0;    // 不关闭线程池

    /*根据线程的最大上限数，给工作线程数组开辟空间，并清零*/
    pool->threads = static_cast<pthread_t*>(malloc((sizeof(pthread_t)*max_thr_num)));
    if(pool->threads==nullptr)
    {
        cout<<"threads malloc failed"<<endl;
        break;
    }
    memset(pool->threads,0,sizeof(pthread_t)*max_thr_num);

    /*任务队列申请空间*/
    pool->task_queue = static_cast<threadpool_task_t *>(malloc(sizeof(threadpool_task_t)*queue_max_size));
    if(pool->task_queue==nullptr)
    {
        cout<<"task_queue malloc failed"<<endl;
        break;
    }
    memset(pool->task_queue,0,sizeof(threadpool_task_t*)*queue_max_size);


    //初始化条件变量，互斥锁 函数成功返回0；任何其他返回值都表示错误
    if (pthread_mutex_init(&(pool->lock), nullptr) != 0
         || pthread_mutex_init(&(pool->thread_counter), nullptr) != 0
            || pthread_cond_init(&(pool->queue_not_empyt), nullptr) != 0
            || pthread_cond_init(&(pool->queue_not_full), nullptr) != 0)
    {
        cout<<"init the lock or cond fail"<<endl;
        break;
    }

    for(i= 0;i<min_thr_num;++i)
    {
      int ret = pthread_create(&pool->threads[i],nullptr,threadpool_thread,static_cast<void*>(pool));
      printf("create thread %x done\n", static_cast<unsigned int>(pool->threads[i]));// 输出线程id
      if(ret)
      {
          cout<<"pthread_create failed"<<endl;
      }
    }
    /*当任务队列为空，子线程将阻塞在子线程的回调函数，而父线程继续执行下面的语句*/
    /*管理线程     子线程的回调函数adjust_thread   运行函数的参数是线程池*/
     cout<<"create manager thread "<<endl;
     pthread_create(&pool->adjust_tid,nullptr,adjust_thread,static_cast<void*>(pool));

    return pool;
    }while(0);

    cout<<"create failed"<<endl;
    threadpool_free(pool);
    return  nullptr;
}
                /*子线程回调函数*/
void * threadpool_thread(void*threadpool)
{
    threadPool_t * pool = static_cast<threadPool_t*>(threadpool);
    threadpool_task_t task;

    while(true)
    {
      /*刚创建出线程，等待任务队列中有任务，否则阻塞，等待任务队列中有任务之后再唤醒任务*/
        pthread_mutex_lock(&pool->lock);

        /*若任务队列中没有任务，线程阻塞在条件变量中*/
        while((pool->queue_size==0) && (!pool->shutdown))
        {
         printf(" thread %x is waiting...\n",static_cast<unsigned int>(pthread_self()));
         pthread_cond_wait(&(pool->queue_not_empyt),&(pool->lock));

        /*清除指定数目的空闲线程，如果要结束的线程个数大于0，则结束线程*/
        if(pool->wait_exit_thr_num>0)
          {
            pool->wait_exit_thr_num--;

           //如果线程池里的线程数个数大于最小值的时候，可以结束当前进程
            if(pool->live_thr_num>pool->min_thr_num)
            {
               printf("thread %x is exiting\n",static_cast<unsigned int>(pthread_self()));
               pool->live_thr_num--;
               pthread_mutex_unlock(&pool->lock);

               pthread_exit(nullptr);//线程退出
            }

          }
        }

        /*如果指定了true，要关闭线程池里的每个线程，自行退出处理---销毁线程池*/
        if (pool->shutdown) {
            pthread_mutex_unlock(&(pool->lock));
            printf("thread %x is exiting\n", static_cast<unsigned int>(pthread_self()));
            pthread_detach(pthread_self());
            pthread_exit(nullptr);     /* 线程自行结束 */
        }

       //从任务队列中获取任务
        task.func = pool->task_queue[pool->queue_front].func;
        task.arg = pool->task_queue[pool->queue_front].arg;

        pool->queue_front = (pool->queue_front+1)%(pool->queue_max_size);
        pool->queue_size--;

        //通知有新的任务可以添加进来
        pthread_cond_broadcast(&pool->queue_not_full);

        //任务取出之后，将线程池锁 释放
        pthread_mutex_unlock(&pool->lock);

        //执行任务
         printf("thread %x start working\n",static_cast<unsigned int>(pthread_self()));
         pthread_mutex_lock(&pool->thread_counter);
         pool->busy_thr_num++;              // 忙线程数++
         pthread_mutex_unlock(&pool->thread_counter);

         /*当前线程执行队头任务
          * 处理完任务之后，再次执行回调函数 看看还有没有其他的任务*/
        (*(task.func))(task.arg);

         //任务结束
         printf(" thread %x finished task ! \n",static_cast<unsigned int>(pthread_self()));

         pthread_mutex_lock(&pool->thread_counter);
         pool->busy_thr_num--;
         pthread_mutex_unlock(&pool->thread_counter);
    }
}
                           /*管理线程*/
#define adjust_waitTime 10
#define MIN_WAIT_TASK_NUM 10
#define add_THREAD 10
#define destry_THREAD 10
void * adjust_thread(void*threadpool)
{
    threadPool_t * pool = static_cast<threadPool_t *>(threadpool);
    while(!pool->shutdown)
    {
        sleep(5);   // 定时对线程池的管理 循环 5s一次

        /*获取线程池的变量值*/
        pthread_mutex_lock(&(pool->lock));
        int queue_size = pool->queue_size;  //任务队列中的任务数
        int live_thr_num = pool->live_thr_num;  // 存活的线程数
        pthread_mutex_unlock(&(pool->lock));

        pthread_mutex_lock(&pool->thread_counter);
       int busy_thr_num = pool->busy_thr_num; // 忙状态的线程数
       pthread_mutex_unlock(&pool->thread_counter);

        /*创建新线程，(任务队列中的任务数/存活的线程数)>80%，且存活的线程数少于最大线程个数时*/

       if((queue_size /live_thr_num)>0.8  && pool->live_thr_num < pool->max_thr_num)
       {
            pthread_mutex_lock(&pool->lock);
             cout<<"pool expend  thread+10"<<endl;
           int add = 0;
           //一次增加DEFAULT_THREAD个线程
           for(int i=0;i<pool->max_thr_num && add<add_THREAD && pool->live_thr_num < pool->max_thr_num;i++)
           {
               pthread_create(&pool->threads[i],nullptr,threadpool_thread,static_cast<void*>(pool));
               add++;
               pool->live_thr_num++;
           }
       }
       pthread_mutex_unlock(&pool->lock);


       /*销毁多余的线程  (忙线程数/存活的线程数)<20% 且存活的线程数大于最小线程*/
       if((busy_thr_num/live_thr_num)<0.2 && live_thr_num > pool->min_thr_num)
       {
           pthread_mutex_lock(&pool->lock);
            printf("pool shrank  thread-10\n");
           pool->wait_exit_thr_num = destry_THREAD;
           pthread_mutex_unlock(&pool->lock);

           for(int i=0;i<destry_THREAD;i++)
           {
               //唤醒空闲的线程，然后销毁
               pthread_cond_signal(&pool->queue_not_empyt);
           }
       }
    }
    return nullptr;
}
/*向线程池 添加一个任务*/
int threadpool_add(threadPool_t * pool,void* (func)(void*),void * arg)
{
    pthread_mutex_lock(&pool->lock);

    //当任务队列已满，调用wait阻塞
    while((pool->queue_size==pool->queue_max_size)&&(!pool->shutdown))
    {
        cout<<"task is full"<<endl;
        pthread_cond_wait(&pool->queue_not_full,&pool->lock);
    }

    if(pool->shutdown)
    {
        pthread_cond_broadcast(&pool->queue_not_empyt);
        pthread_mutex_unlock(&pool->lock);
        return 0;
    }
    /*清空 工作线程调用的回调函数的参数 */
    if(pool->task_queue[pool->queue_rear].arg!=nullptr)
    {
        pool->task_queue[pool->queue_rear].arg=nullptr;
    }
    /*添加任务到任务队列*/
    pool->task_queue[pool->queue_rear].func = func;
    pool->task_queue[pool->queue_rear].arg = arg;
    pool->queue_rear = (pool->queue_rear+1)%pool->queue_max_size;
    pool->queue_size++;   // 实际任务数++

    /*添加完任务之后，任务队列不为空，唤醒线程池的线程*/
    pthread_cond_signal(&pool->queue_not_empyt);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}
/*线程池的线程，模拟处理业务*/
void *process(void * arg)
{
     printf("thread  %x  working on task %d\n",
    static_cast<unsigned int>(pthread_self()),*(static_cast<int*>(arg)));
     sleep(1);
    printf("task %d is end\n",*(static_cast<int*>(arg)));
     return nullptr;
}
/*销毁线程池的变量*/
int threadpool_free(threadPool_t*pool)
{
    cout<<"threadpool_free"<<endl;
    if(pool==nullptr)
    {
        return 0;
    }
    if(pool->task_queue)
    {
        free(pool->task_queue);
    }
    if(pool->threads)
    {
        free(pool->threads);
        pthread_mutex_lock(&pool->lock);
        pthread_mutex_destroy(&pool->lock);
        pthread_mutex_lock(&pool->thread_counter);
        pthread_mutex_destroy(&pool->thread_counter);
        pthread_cond_destroy(&pool->queue_not_full);
        pthread_cond_destroy(&pool->queue_not_empyt);
    }
    free(pool);
    pool = nullptr;
    return 0;
}
int threadpool_destory(threadPool_t*pool)
{
    cout<<"threadpool_destory"<<endl;
    int i;
    if(pool == nullptr)
    {
        return -1;
    }
    pool->shutdown = 1;

    /*先销毁管理线程
    pthread_join()函数，以阻塞的方式等待thread指定的线程结束。当函数返回时，被等待线程的资源被收回
   */
    pthread_join(pool->adjust_tid,nullptr);

    for(i=0;i<pool->live_thr_num;i++)
    {
        /*通知所有的空闲线程 意味着线程都会死亡
         实质是唤醒被阻塞的空闲进程，然后销毁空闲的进程阻塞在queue_not_empyt条件变量中*/
        pthread_cond_broadcast(&pool->queue_not_empyt);
    }

    for(i=0;i<pool->live_thr_num;i++)
    {
        pthread_join(pool->threads[i],nullptr);
    }

    threadpool_free(pool);

    return 0;
}
int main()
{

    threadPool_t * thp = threadpool_create(5,100,500);
    cout<<"threadpool_create done"<<endl;

    int num[50],i;
    for(i=0;i<50;i++)
    {
        num[i]=i;
        cout<<"add task:"<<i<<endl;
        /*向线程池添加任务   process回调函数表示处理任务*/
        threadpool_add(thp,process,static_cast<void*>(&num[i])); //(void*)&num[i]回调函数的传参
    }

    sleep(20);
    threadpool_destory(thp);
    cout<<"threadpool destory done"<<endl;

    return 0;
}
