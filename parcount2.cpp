	/*Created by Sayak Chakraborti NetID-schakr11,Feb 2017 
compile- g++ parcount2.cpp -w -std=c++0x -pthread
Run - ./parcount2 -t 4 -i 10000
NOTE:-
fences- ||RW at end of acquire is the acquire fence :it ensures that the
lock is held (the preceding spin has completed successfully) before the thread can execute any instructions in the
critical section (the code that will follow the return).
 Similarly, the RW|| store in release is a “release access”: it
ensures that all instructions in the critical section (the code that preceded the call) have completed before the lock
is released.
                       release
                         |
                         v
  -------------------------------------
  | load->load(RR) |  load->store(RW) | <- acquire
  |----------------|------------------|
  | store->load(WR)| store->store(WW) |
  -------------------------------------
*/
#include <thread>
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include <atomic>
#include <mutex>
#include <ctime>
#include <chrono>
#include <algorithm>
#include <utility>
using namespace std;

/*Global variables for locks and time calculation*/
std::atomic<bool> start;

std::mutex cpp_lock;                         //CPP lock
/*---------------Test and Set----------------*/
/////////
std::atomic_flag n_TAS=ATOMIC_FLAG_INIT;   //naive test and set lock

std::atomic_flag wt_TAS=ATOMIC_FLAG_INIT;   //well tuned test and set lock
 
 int wt_tas_base=25;
 int wt_tas_limit=1;
 int wt_tas_multiplier=5;
 int wt_tas_delay=25;

/////////

/*---------------Ticket Lock----------------*/
std::atomic<int> Tlock_next_ticket(0);    //Ticket lock
std::atomic<int> Tlock_now_serving(0);
int Tlock_base=10;                        //Tuned ticket lock

/////////

/*---------------MCS Lock----------------*/

//for MCS lock
struct qnode
{
std::atomic<struct qnode*> next;
std::atomic<bool> waiting;
};

std::atomic<struct qnode*> tail;


/*---------------K42 MCS----------------*/

//for K42-MCS 

struct K42_MCS_qnode
{

std::atomic<struct K42_MCS_qnode*> tail;

std::atomic<struct K42_MCS_qnode*> next;
      
};

 //std::atomic<struct K42_MCS_qnode*> waiting;
//std::atomic<uintptr_t> waiting;
struct K42_MCS_qnode q;

//struct K42_MCS_qnode* waiting;
 bool waiting;

 /*---------------CLH----------------*/
    
//for CLH 

struct CLH_qnode
{

std::atomic<struct CLH_qnode*> prev;
std::atomic<bool> succ_must_wait;


};

struct CLH_qnode dummy;

std::atomic<struct CLH_qnode*> clh_tail;

/*---------------K42 CLH----------------*/
 
//for K42 CLH
struct K42_CLH_qnode
{
std::atomic<bool> succ_must_wait;
};

struct K42_CLH_qnode dummy_k42;
std::atomic<struct K42_CLH_qnode*> k42_clh_tail;
std::atomic<struct K42_CLH_qnode*> k42_clh_head;

struct K42_CLH_qnode initial_thread_qnodes[100];
struct K42_CLH_qnode* thread_qnode_ptrs[100];

/*---------------------------------------------------------*/
//for time keeping
std::chrono::high_resolution_clock::time_point t1,t2;
std::chrono::duration<double> time_span;
/*....*/

void make_threads_joinsAndReLaunch(int num_threads,int iterations);
void my_pause(int k);
void MCS_acquire(struct qnode* ptr);
void MCS_release(struct qnode* ptr);

/*---------------------------Acquire and release locks-----------------*/

//MCS

void MCS_acquire(struct qnode* p)
{
        
               
        p->next.store(NULL);                           //initializing the qnode for the thread which is trying to acquire the lock
         p->waiting.store(true);                       
         
          struct qnode* prev;
          
         prev=tail.exchange(p,std::memory_order_release);       //place this qnode in the linked list(pointed to by tail the last element) 
    
             if(prev!=NULL)                                          //queue was non-empty
                {
                  prev->next.store(p);     //let the previous qnode of the previous thread who currently has the lock know that you are next
                  while(p->waiting.load());  //spin until the prevoious node sets you up once it has released

                 }
           //if queue was empty and we are the only ones then we have the lock
               
}

void MCS_release(struct qnode* p)
{
   struct qnode* succ;
  succ=p->next.load(std::memory_order_acquire);    //find my successor
  
  if(succ==NULL)                        //if there is no successor in the queue
  {
    struct qnode* ptr;
         ptr=p;                  //it may so happen that a node thread enters the queue after I have checked for emptyness

        if(tail.compare_exchange_strong(ptr,NULL,memory_order_release,memory_order_acquire))  //so try to CAS tail to NULL
           return;                                          //if successful return 

           while(1)
            {
               succ=p->next.load();                //if not wait for the incomming thread to set my next pointer to no1 NULL and then I can transfer to him

              
                if(succ!=NULL)
                 break;
            }

  }
 
 succ->waiting.store(false,std::memory_order_release);         //now the successor can start
 
}



//K42 CLH
void K42_CLH_acquire(int self)
{
  struct K42_CLH_qnode* p;
  p=thread_qnode_ptrs[self];
  p->succ_must_wait.store(true);   //indicating that the next waiting thread shooudl spin
  struct K42_CLH_qnode* pred;
   
  pred=k42_clh_tail.exchange(p,std::memory_order_release);  //set tail to point to this new node

  while(pred->succ_must_wait.load()); //spin wait on predecessor's flag
  //lock acquired
  k42_clh_head.store(p,memory_order_acquire); //store this as head
  thread_qnode_ptrs[self]=pred;  //change the thread's array pointer to the predecessor

}


void K42_CLH_release()
{
 (*k42_clh_head).succ_must_wait.store(false,memory_order_release);
}


//CLH
void CLH_acquire(struct CLH_qnode* ptr)
{

  ptr->succ_must_wait.store(true);   //set ittotrue so that the next successor has to wait before you complete
 struct CLH_qnode* pred;
 
   pred=ptr->prev=(clh_tail).exchange(ptr,memory_order_release); //get pointer to predecessor using swap
  

   while(pred->succ_must_wait.load(memory_order_acquire));   //wait on predecessors to complete



}

void CLH_release(struct CLH_qnode** ptr)
{

  struct CLH_qnode* pred;
   pred=(*ptr)->prev;         //get previous pointer
   (*ptr)->succ_must_wait.store(false,memory_order_release); //set it's own successor must wait to false(so that the successor can get into it's cs)
   (*ptr)=pred;	//change current pointer to previous pointer

}

//K42 MCS
void MCS_K42_acquire()
{

   while(1)
   {  //q is the lock
     struct K42_MCS_qnode* prev;

      prev=q.tail.load();    //get the previous qnode

      struct K42_MCS_qnode* p;
                                   //using the pointer of q
        p=NULL;

    	  if(prev==NULL)
      	  {  //use compare and swap to replace the tail pointer (which is null for the first acquire) with the pointer to the lock itself
            if((q.tail).compare_exchange_strong(p,&q,memory_order_release,memory_order_acquire))    //CAS(&q.tail,NULL,&q)
               break;
          }
          else
          {   //if previous thread present
                 struct K42_MCS_qnode n;
                  n.tail.store((struct K42_MCS_qnode*)waiting);
                  n.next.store(NULL);
                
                   struct K42_MCS_qnode* ptr1;
                    ptr1=prev;
                         //compare and exchange the tail with local pointer n value if tail still has the prev pointer value(nobody came in between)
                   if((q.tail).compare_exchange_strong(ptr1,&n,memory_order_release,memory_order_acquire)) //CAS(&q.tail,prev,&n,W||)
                          {	
                                //if successful the q.tail contains pointer to the new node n
                                 prev->next.store(&n);
                                
                                   while(n.tail.load()==(struct K42_MCS_qnode*)waiting);           //wait for the lock //wait on tail
                            
                                 //now we have the lock
                               


                                     struct K42_MCS_qnode* succ;
                                       succ=n.next.load();             //find my successor
                                  
                                         if(succ==NULL)        //if successor is NULL then 
                                          {
 
                                            q.next.store(NULL);  //there is no one else waiting so set the next pointer of the lock to NULL
                                            //trying to make lock point at itself
                                                  struct K42_MCS_qnode* ptr2;
                                                    ptr2=&n;
                                                 if(!((q.tail).compare_exchange_strong(ptr2,&q,memory_order_release,memory_order_acquire))  )  //~CAS(&q.tail,&n,&q)
                                                   {  
                                                       //somebody got into the timing window
                                                        //CAS failure someone requested acquire after I checked for successor and found NULL 
                                                        while(1)               
                                                        {
                                                          succ=n.next.load();            //wait for the requesting thread to set next pointer to itself
                                                          
                                                              if(succ!=NULL)
                                                                break;

                                                         } //while close

                                                       q.next.store(succ,memory_order_acquire); //store successor as next

                                                    }  //if ~CAS close

                                             break;
                                          } //if succ close
                                         else
                                          {

                                      
                                               q.next.store(succ,memory_order_acquire);
                                               break;



                                          }//else close
                          }//if CAS close




         }

   }//while close


}


void MCS_K42_release()
{

   struct K42_MCS_qnode* succ;

  // std::atomic<struct K42_MCS_qnode*> succ;
   succ=q.next.load(memory_order_release);   //gives the next waiting thread
  
    if(succ==NULL)  //if no next waiting thread
    {
        struct K42_MCS_qnode* ptr;
        
         ptr=&q;                  //not doing it right
  
         //CAS NULL into tail
          if((q.tail).compare_exchange_strong(ptr,NULL,memory_order_release,memory_order_acquire)) 
            return;  //return if success ,no one requested the acquire

             while(1)                  //on failure someone requested the acquire after I check successor for NULL
             {

                succ=q.next.load();        //wait till successor is found when it sets itselfin the queue of tail
               
                if(succ!=NULL)
                   break;

              }


    }

  succ->tail.store(NULL);   //release waiting on tail


}



/*-----------------------------functions using the acquire and release------------------*/
void funcCLH(int & count ,int iter)
{
   while (!start.load());//spin until start becomes true.This is done so that all threads start at the same time (truely run parallely)

int i;
  for(i=0;i<iter;i++)
    {
  
        struct CLH_qnode* p;
        //struct CLH_qnode* ptr;
         p=(struct CLH_qnode*)malloc(sizeof(struct CLH_qnode));

         CLH_acquire(p);
          
           // printf("\n");

            count=count+1;
          //ptr=&p;
   
       CLH_release(&p);
   }//for close
}


void funcK42_CLH(int & count ,int iter,int self)
{

    while (!start.load());//spin until start becomes true.This is done so that all threads start at the same time (truely run parallely)

int i;
  for(i=0;i<iter;i++)
    {
  
        struct CLH_qnode p;
        struct CLH_qnode* ptr;


         K42_CLH_acquire(self);
          
           // printf("\n");

            count=count+1;
           ptr=&p;
   
       K42_CLH_release();
   }//for close



}

void funcK42_MCS(int & count ,int iter)
{
   while (!start.load());//spin until start becomes true.This is done so that all threads start at the same time (truely run parallely)

int i;
  for(i=0;i<iter;i++)
    {
  
       // struct qnode p;

         MCS_K42_acquire();
          
           // printf("\n");

            count=count+1;
  
   
       MCS_K42_release();
   }//for close
}



void funcMCS(int & count ,int iter)
{
   while (!start.load());//spin until start becomes true.This is done so that all threads start at the same time (truely run parallely)

int i;
  for(i=0;i<iter;i++)
    {
  
        struct qnode p;

         MCS_acquire(&p);
          
           // printf("\n");

            count=count+1;
  
   
       MCS_release(&p);
   }//for close
}



//naive ticket lock
void func_naiveTicketLock(int & count,int iter)
{

  while (!start.load());//spin until start becomes true.This is done so that all threads start at the same time (truely run parallely)


  int i;
  for(i=0;i<iter;i++)
    {
          //ticket lock acquire

          int myTicket=Tlock_next_ticket.fetch_add(1,std::memory_order_acquire);  //get my ticket number in atomic manner 
          while(1)
          {
              int ns=Tlock_now_serving.load(std::memory_order_acquire);  //loads the current serving ticket

                if(ns==myTicket)  //matches whether it's his ticket
                break;


            }  

              count=count+1;
 
              //ticket lock release
              int temp=Tlock_now_serving+1;
               Tlock_now_serving.store(temp,std::memory_order_release);//store the next ticket to be served
    }
}



//well tuned ticket lock
void func_wtTicketLock(int & count,int iter)
{

  while (!start.load());//spin until start becomes true.This is done so that all threads start at the same time (truely run parallely)

int i;
  for(i=0;i<iter;i++)
    {
  
        int myTicket=Tlock_next_ticket.fetch_add(1,std::memory_order_acquire); //get my ticket
        while(1)
        {
            int ns=Tlock_now_serving.load(std::memory_order_acquire);  //load now serving

            if(ns==myTicket)                   //check whether my ticket is now serving
              break;

           my_pause(Tlock_base*(myTicket-ns));   //delay the next iteration of checking

       }  
        //lock acquired

        count=count+1;
 
         //lock release
          int temp=Tlock_now_serving+1;
        Tlock_now_serving.store(temp,std::memory_order_release);
    }
}


//counter increment function with specfic lock mechanism on a mutex,so that at a time only one thread gets to increment counter at a time
void func_lock( int & count,int iter)
{

	while (!start.load());//spin until start becomes true.This is done so that all threads start at the same time (truely run parallely)

	int i;
	for(i=0;i<iter;i++)
   	{
     		cpp_lock.lock();   //acquire lock on the mutex _lock
     		count=count+1;
     		cpp_lock.unlock(); //release lock on the mutex _lock
   	} 
}


//naive test and set lock
void func_naiveTAS( int & count,int iter)
{

  while (!start.load());//spin until start becomes true.This is done so that all threads start at the same time (truely run parallely)

  int i;

  for(i=0;i<iter;i++)
    {
       while(n_TAS.test_and_set(std::memory_order_acquire));  //if true then spin //set the lock
                                    
        count=count+1;
        n_TAS.clear(std::memory_order_release); //release log by clearing flag,means settingit to false
    } 
}



//naive test and set lock
void func_wtTAS( int & count,int iter)
{

  while (!start.load());//spin until start becomes true.This is done so that all threads start at the same time (truely run parallely)

  int i;

  for(i=0;i<iter;i++)
    {
       while(n_TAS.test_and_set(std::memory_order_acquire))  //acquire lock by setting the variable or spinning on it
        { my_pause(wt_tas_delay);

         wt_tas_delay=((wt_tas_delay*wt_tas_multiplier)>wt_tas_limit)?wt_tas_limit:(wt_tas_delay*wt_tas_multiplier);
        }
         count=count+1;
        
        n_TAS.clear(std::memory_order_release); //release log by clearing flag,means setting it to false
    } 
}

void my_pause(int k)
{
int i;
  for(i=0;i<k;i++)
  {

  }

}


/*------------------------------main method--------------------------------*/

int main(int argc,char** argv)
{
	int num_threads;
	int iterations;

    num_threads=4;
    iterations=10000;

   
	if(argc!=0)
	{
			if(argc==5)
			{   
          
					if(strcmp(argv[1],"-t")==0)
          		    		{ num_threads=atoi(argv[2]);

              				}

              				if(strcmp(argv[3],"-t")==0)
              				{ num_threads=atoi(argv[4]);

              				}


              				if(strcmp(argv[1],"-i")==0)
              				{ iterations=atoi(argv[2]);

              				}

              				if(strcmp(argv[3],"-i")==0)
              				{ iterations=atoi(argv[4]);

              				}

			}//if argc ==5 close

			if(argc==3)
			{
            

            	 		if(strcmp(argv[1],"-t")==0)
              			{ num_threads=atoi(argv[2]);

              			}

            			if(strcmp(argv[1],"-i")==0)
              			{ iterations=atoi(argv[2]);

              			}

			}//if argc==3 close
			
    }//if argc!=0 close
	
    make_threads_joinsAndReLaunch(num_threads,iterations);
  		return 0;
}

//Supervisor function that makes the threads and allotes them different task for each experiment and relaunches them with initial conditions for different experiments
void make_threads_joinsAndReLaunch(int num_threads,int iterations)
{
     
	     int counter=0,i;double increments_per_micro=0.0;
        std::thread myThreads[num_threads];      //initializing  threads

     
        //lock and unlock mechanism
        counter=0;
        start=false; //initialize start to false so that each thread waits for every other thread to be created before it can start
       
        for ( i=0; i<num_threads; i++)
        { myThreads[i]=std::thread(func_lock, std::ref(counter),iterations);  //run N number of threads
        
        } 

        start=true;  //all threads start now
    
        t1=std::chrono::high_resolution_clock::now();   //get current time with fine granularity(start time for threads)

        for ( i=0; i<num_threads; i++)
        myThreads[i].join();                               //wait for all the threads created to complete

        t2=std::chrono::high_resolution_clock::now();    //get current time when the threads have terminated

        time_span=std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); //calculate the difference of the two times to get the time required to execute

        cout <<"CPP Lock "<< counter << " Time " << time_span.count()<<" secs" << endl;
        increments_per_micro=((double)counter)/((double)time_span.count());
       increments_per_micro=increments_per_micro/1000000.0;
       cout<<" Increments per micro "<< increments_per_micro << endl; 


  
      /*---------------------------------------------------------*/
      

        //naive test and set
        counter=0;
        start=false; //initialize start to false so that each thread waits for every other thread to be created before it can start
        //flag_n_TAS=false;

        for ( i=0; i<num_threads; i++)
        { myThreads[i]=std::thread(func_naiveTAS, std::ref(counter),iterations);  //run N number of threads
        
        } 

        start=true;  //all threads start now
    
        t1=std::chrono::high_resolution_clock::now();   //get current time with fine granularity(start time for threads)

        for ( i=0; i<num_threads; i++)
        myThreads[i].join();                               //wait for all the threads created to complete

        t2=std::chrono::high_resolution_clock::now();    //get current time when the threads have terminated

        time_span=std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); //calculate the difference of the two times to get the time required to execute

        cout <<"naive TAS "<< counter << " Time " << time_span.count()<<" secs" << endl;
        increments_per_micro=((double)counter)/((double)time_span.count());
        increments_per_micro=increments_per_micro/1000000.0;
        cout<<" Increments per micro "<< increments_per_micro << endl; 
      /*--------------------------------------------------------*/
     
        counter=0;
        start=false; //initialize start to false so that each thread waits for every other thread to be created before it can start
       
        for ( i=0; i<num_threads; i++)
        { myThreads[i]=std::thread(func_wtTAS, std::ref(counter),iterations);  //run N number of threads
        
        } 

        start=true;  //all threads start now
    
        t1=std::chrono::high_resolution_clock::now();   //get current time with fine granularity(start time for threads)

        for ( i=0; i<num_threads; i++)
        myThreads[i].join();                               //wait for all the threads created to complete

        t2=std::chrono::high_resolution_clock::now();    //get current time when the threads have terminated

        time_span=std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); //calculate the difference of the two times to get the time required to execute

        cout <<"well tuned TAS "<< counter << " Time " << time_span.count()<<" secs" << endl;
        increments_per_micro=((double)counter)/((double)time_span.count());
        increments_per_micro=increments_per_micro/1000000.0;
        cout<<" Increments per micro "<< increments_per_micro << endl; 
      
        /*--------------------------------------------------------*/
         //MCS
          tail=NULL;
       
          counter=0;
          start=false; //initialize start to false so that each thread waits for every other thread to be created before it can start
       
            for ( i=0; i<num_threads; i++)
            { myThreads[i]=std::thread(funcMCS, std::ref(counter),iterations);  //run N number of threads
        
            } 

            start=true;  //all threads start now
    
            t1=std::chrono::high_resolution_clock::now();   //get current time with fine granularity(start time for threads)

            for ( i=0; i<num_threads; i++)
            myThreads[i].join();                               //wait for all the threads created to complete

            t2=std::chrono::high_resolution_clock::now();    //get current time when the threads have terminated

            time_span=std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); //calculate the difference of the two times to get the time required to execute

            cout <<"MCS   "<< counter << " Time " << time_span.count()<<" secs" << endl;
            increments_per_micro=((double)counter)/((double)time_span.count());
            increments_per_micro=increments_per_micro/1000000.0;
            cout<<" Increments per micro "<< increments_per_micro << endl; 

       /*-------------------------------------------------------------------*/
        //K42 MCS
           waiting=1;
        
            q.next.store(NULL);
            q.tail.store(NULL);

      
          counter=0;
          start=false; //initialize start to false so that each thread waits for every other thread to be created before it can start
       
          for ( i=0; i<num_threads; i++)
          { myThreads[i]=std::thread(funcK42_MCS, std::ref(counter),iterations);  //run N number of threads
        
          } 

            start=true;  //all threads start now
    
          t1=std::chrono::high_resolution_clock::now();   //get current time with fine granularity(start time for threads)

            for ( i=0; i<num_threads; i++)
            myThreads[i].join();                               //wait for all the threads created to complete

            t2=std::chrono::high_resolution_clock::now();    //get current time when the threads have terminated

            time_span=std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); //calculate the difference of the two times to get the time required to execute

            cout <<"K42_MCS   "<< counter << " Time " << time_span.count()<<" secs" << endl;
            increments_per_micro=((double)counter)/((double)time_span.count());
            increments_per_micro=increments_per_micro/1000000.0;
            cout<<" Increments per micro "<< increments_per_micro << endl; 

         /*---------------------------------------------------------------------*/
          //K42 CLH

               /*struct K42_CLH_qnode dummy_k42;
                  std::atomic<struct K42_CLH_qnode*> k42_clh_tail;
                  struct K42_CLH_qnode initial_thread_qnodes[100];
                  struct K42_CLH_qnode* thread_qnode_ptrs[100];
              */

            dummy_k42.succ_must_wait.store(false);
            k42_clh_tail.store(&dummy_k42);        //same problem here
    
            for(i=0;i<100;i++)
            {
        
                thread_qnode_ptrs[i]=&initial_thread_qnodes[i];
            }
           //pass i to acquire and release

     
            counter=0;
            start=false; //initialize start to false so that each thread waits for every other thread to be created before it can start
       
            for ( i=0; i<num_threads; i++)
            { myThreads[i]=std::thread(funcK42_CLH, std::ref(counter),iterations,i);  //run N number of threads
        
            } 

            start=true;  //all threads start now
    
            t1=std::chrono::high_resolution_clock::now();   //get current time with fine granularity(start time for threads)

              for ( i=0; i<num_threads; i++)
                myThreads[i].join();                               //wait for all the threads created to complete

            t2=std::chrono::high_resolution_clock::now();    //get current time when the threads have terminated

            time_span=std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); //calculate the difference of the two times to get the time required to execute

            cout <<"K42 CLH   "<< counter << " Time " << time_span.count()<<" secs" << endl;
            increments_per_micro=((double)counter)/((double)time_span.count());
            increments_per_micro=increments_per_micro/1000000.0;
            cout<<" Increments per micro "<< increments_per_micro << endl; 
        /*-----------------------------------------------------------------------*/
        //CLH

            dummy.prev.store(NULL);
            dummy.succ_must_wait.store(false);

            clh_tail.store(&dummy);                   //unable to store atomic<clh_qnode> dummy to atomic<clh_qnode*>clh_tail
         
            counter=0;
            start=false; //initialize start to false so that each thread waits for every other thread to be created before it can start
       
              for ( i=0; i<num_threads; i++)
              { myThreads[i]=std::thread(funcCLH, std::ref(counter),iterations);  //run N number of threads
        
                } 

              start=true;  //all threads start now
    
             t1=std::chrono::high_resolution_clock::now();   //get current time with fine granularity(start time for threads)

              for ( i=0; i<num_threads; i++)
                myThreads[i].join();                               //wait for all the threads created to complete

              t2=std::chrono::high_resolution_clock::now();    //get current time when the threads have terminated

            time_span=std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); //calculate the difference of the two times to get the time required to execute

              cout <<"CLH   "<< counter << " Time " << time_span.count()<<" secs" << endl;
              increments_per_micro=((double)counter)/((double)time_span.count());
              increments_per_micro=increments_per_micro/1000000.0;
              cout<<" Increments per micro "<< increments_per_micro << endl; 
      
              /*--------------------------------------------------------*/
        //naive ticket lock
        counter=0;
        start=false; //initialize start to false so that each thread waits for every other thread to be created before it can start
       
        for ( i=0; i<num_threads; i++)
        { myThreads[i]=std::thread(func_naiveTicketLock, std::ref(counter),iterations);  //run N number of threads
        
        } 

        start=true;  //all threads start now
    
        t1=std::chrono::high_resolution_clock::now();   //get current time with fine granularity(start time for threads)

        for ( i=0; i<num_threads; i++)
        myThreads[i].join();                               //wait for all the threads created to complete

        t2=std::chrono::high_resolution_clock::now();    //get current time when the threads have terminated

        time_span=std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); //calculate the difference of the two times to get the time required to execute

        cout <<"naive Ticket Lock  "<< counter << " Time " << time_span.count()<<" secs" << endl;
        increments_per_micro=((double)counter)/((double)time_span.count());
        increments_per_micro=increments_per_micro/1000000.0;
        cout<<" Increments per micro "<< increments_per_micro << endl; 
 

        /*--------------------------------------------------------*/
         //well tuned ticket lock
        counter=0;
        start=false; //initialize start to false so that each thread waits for every other thread to be created before it can start
       
        for ( i=0; i<num_threads; i++)
        { myThreads[i]=std::thread(func_wtTicketLock, std::ref(counter),iterations);  //run N number of threads
        
        } 

        start=true;  //all threads start now
    
        t1=std::chrono::high_resolution_clock::now();   //get current time with fine granularity(start time for threads)

        for ( i=0; i<num_threads; i++)
        myThreads[i].join();                               //wait for all the threads created to complete

        t2=std::chrono::high_resolution_clock::now();    //get current time when the threads have terminated

        time_span=std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); //calculate the difference of the two times to get the time required to execute

        cout <<"Well tuned Ticket Lock  "<< counter << " Time " << time_span.count()<<" secs" << endl;
        increments_per_micro=((double)counter)/((double)time_span.count());
        increments_per_micro=increments_per_micro/1000000.0;
        cout<<" Increments per micro "<< increments_per_micro << endl; 
       


      
 
           
}

/*
void MCS_acquire(struct qnode* p)
{
        std::atomic<struct qnode*> ptr;
        ptr.store(p); 
               
         ptr.load()->next.store(NULL);
           ptr.load()->waiting.store(true);
         
          struct qnode* prev;
         prev=tail.exchange(ptr.load(),std::memory_order_release);
    
             if(prev!=NULL)                                          //queue was non-empty
                {
                  prev->next.store(ptr.load());
                  while(ptr.load()->waiting.load());  //spin

                 }
           
               
}

void MCS_release(struct qnode* p)
{
   std::atomic<struct qnode*> ptr;
        ptr.store(p);   
   struct qnode* succ;
  succ=ptr.load()->next.load(std::memory_order_acquire);    //std::memory_order_acq_rel 
  
  if(succ==NULL)
  {

        if(tail.compare_exchange_strong(p,NULL,memory_order_release,memory_order_acquire))
           return;

           while(1)
            {
               succ=ptr.load()->next.load();

              
                if(succ!=NULL)
                 break;
            }

  }
 
 succ->waiting.store(false,std::memory_order_release);
 
}
*/
