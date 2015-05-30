#include "VirtualMachine.h"
#include "Machine.h"
#include <unistd.h>
#include <iostream>
#include <vector>
#include <queue>
#include <list>
#include <string.h>
#include <fcntl.h>

using namespace std;

extern "C"
{    
    void FileCallback(void *calldata, int result);
    typedef struct
    {
        TVMThreadEntry entry;    //for the tread entry function 
        SMachineContext context;//for the context to switch to/from the thread
        TVMThreadID Thread_ID;    //to hold ID
        TVMThreadPriority ThreadPriority;    //for thread priority
        TVMThreadState ThreadState;    //for thread stack
        TVMMemorySize MemorySize;    //for stack size
        uint8_t *BaseStack;        //pointer for base of stack
        void *ThreadParameter;    // for thread entry parameter
        TVMTick ticks;            // for the ticcks that thread needs to wait
        int file;
        TVMMutexID MyMutex;
        int MutexPrioIndex;
        int SleepingIndex;
        TVMMemorySize memsize;
    }TCB; 

	typedef struct
	{
		TVMMutexID MutexID;
		TVMThreadID OwnerID;
        bool unlocked;
        vector<TCB*> HighPrio;
        vector<TCB*> NormalPrio;
        vector<TCB*> LowPrio;    

	}mutex;

    typedef struct 
    {
        uint8_t *address;
        TVMMemorySize length; 
    }block;

    //list of freelist and allocated list 
    // free space = length of the amount of free space you have in freelist
    typedef struct 
    {
        TVMMemorySize MemoryPoolSize;
        TVMMemoryPoolID PoolID;
        //pointer to the base of memory array
        uint8_t* base;
        int length;
        //make free space stuff
        TVMMemorySize FreeSpace;
        list<block*> FreeList;
        list<block*> AllocatedList;

    }MemoryPool;

    typedef struct 
    {/*
        directory stuff
      */  
        
    }directory;

    typedef struct 
    {
        /* data */
    }BootBlock;

    const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 0;

    const TVMMemoryPoolID VM_MEMORY_POOL_ID_SHARED = 1;

    volatile TVMThreadID CurrentThreadIndex;

    vector<MemoryPool*> MemoryIDVector;
	vector<mutex*> MutexIDVector;
    vector<TCB*> ThreadIDVector;
    vector<TCB*> LowQueue;
    vector<TCB*> NormalQueue;
    vector<TCB*> HighQueue;
    vector<TCB*> WaitingQueue;
    vector<TCB*> SleepingQueue;
    vector<TCB*> HighWaitQueue;
    vector<TCB*> NormalWaitQueue;
    vector<TCB*> LowWaitQueue;

    TVMMainEntry VMLoadModule(const char *module);

    void IdleEntry(void *param)
    {
        MachineEnableSignals();
        while(true)
        {
        }
    }

    void SkeletonFunction(void *param)
    {
        MachineEnableSignals();
        ThreadIDVector[CurrentThreadIndex]->entry(ThreadIDVector[CurrentThreadIndex]->ThreadParameter);
        VMThreadTerminate(CurrentThreadIndex);
    }


    void PlaceIntoMutexQueue(TVMThreadID thread, TVMMutexID mutex)
    {
            switch(ThreadIDVector[thread]->ThreadPriority)
            {
                case VM_THREAD_PRIORITY_LOW:                    
                    ThreadIDVector[thread]->MutexPrioIndex = MutexIDVector[mutex]->LowPrio.size();
                    MutexIDVector[mutex]->LowPrio.push_back(ThreadIDVector[thread]);
                    break;
                case VM_THREAD_PRIORITY_NORMAL:
                    ThreadIDVector[thread]->MutexPrioIndex = MutexIDVector[mutex]->NormalPrio.size();
                    MutexIDVector[mutex]->NormalPrio.push_back(ThreadIDVector[thread]);
                    break;
                case VM_THREAD_PRIORITY_HIGH:
                    ThreadIDVector[thread]->MutexPrioIndex = MutexIDVector[mutex]->HighPrio.size();
                    MutexIDVector[mutex]->HighPrio.push_back(ThreadIDVector[thread]);
                    break;
            }
    }

    //checks what the current treadstate is and if it is ready puts it in proper queue
    //place into ready queue
    void PlaceIntoQueue(TVMThreadID thread)
    {
        if(ThreadIDVector[thread]->ThreadState == VM_THREAD_STATE_READY)
        {
            switch(ThreadIDVector[thread]->ThreadPriority)
            {
                case VM_THREAD_PRIORITY_LOW:    
                    LowQueue.push_back(ThreadIDVector[thread]);
                    break;
                case VM_THREAD_PRIORITY_NORMAL:
                    NormalQueue.push_back(ThreadIDVector[thread]);
                    break;
                case VM_THREAD_PRIORITY_HIGH:
                    HighQueue.push_back(ThreadIDVector[thread]);
                    break;
            }
        }
    }

    //when sharedmemory is full, puts it in waitingqueue
    //checks what the treadstate is and if it is waiting puts it in proper queue
    //place into waiting queue
    void PlaceIntoWaitQueue(TVMThreadID thread)
    {
        if(ThreadIDVector[thread]->ThreadState == VM_THREAD_STATE_WAITING)
        {
            //cerr<<endl<<"enter wait queu "<<thread<<endl;
            switch(ThreadIDVector[thread]->ThreadPriority)
            {
                case VM_THREAD_PRIORITY_LOW:    
                    LowWaitQueue.push_back(ThreadIDVector[thread]);
                    break;
                case VM_THREAD_PRIORITY_NORMAL:
                    NormalWaitQueue.push_back(ThreadIDVector[thread]);
                    break;
                case VM_THREAD_PRIORITY_HIGH:
                    HighWaitQueue.push_back(ThreadIDVector[thread]);
                    break;
            }
        }
    }

    //used for shared memory when queue frees up puts it in ready
    //if queue not empty move the thread from waiting to ready
    void WaitToReady()
    {
        TVMThreadID tid;
        if(!HighWaitQueue.empty())
        {
            tid = HighWaitQueue.front()->Thread_ID;
            HighWaitQueue.erase(HighWaitQueue.begin());
            ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_READY;
            PlaceIntoQueue(tid);
        }
        else if(!NormalWaitQueue.empty())
        {
            tid = NormalWaitQueue.front()->Thread_ID;
            NormalWaitQueue.erase(NormalWaitQueue.begin());
            ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_READY;
            //cerr<<endl<<"exit wait queue "<<tid<<endl;
            PlaceIntoQueue(tid);
        }
        else if(!LowWaitQueue.empty())
        {
            tid = LowWaitQueue.front()->Thread_ID;
            LowWaitQueue.erase(LowWaitQueue.begin());
            ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_READY;
            PlaceIntoQueue(tid);
        }
    }

    //Check wait queues to see if there is currently enough free shared space to run the waiting threads
    void CheckForFreeSharedSpace()
    {
        TVMThreadID tid;
        list<block*>::iterator it;
        //if high wait queue not empty
        if(!HighWaitQueue.empty())
        {
            //from beginning of freelist to end of freelist
            for(it = MemoryIDVector[VM_MEMORY_POOL_ID_SHARED]->FreeList.begin(); it != MemoryIDVector[VM_MEMORY_POOL_ID_SHARED]->FreeList.end();it++)
            {
                //if legnth==memory size set thread id equal to the front of the queue, get rid of what is in from and move it to ready queue.
                if((*it)->length >= HighWaitQueue.front()->memsize)
                {
                    tid = HighWaitQueue.front()->Thread_ID;
                    HighWaitQueue.erase(HighWaitQueue.begin());
                    ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_READY;
                    PlaceIntoQueue(tid);
                    //cerr<<endl<<"exit wait queue"<<tid<<endl;
                }
            }
        }
        else if(!NormalWaitQueue.empty())
        {
            for(it = MemoryIDVector[VM_MEMORY_POOL_ID_SHARED]->FreeList.begin(); it != MemoryIDVector[VM_MEMORY_POOL_ID_SHARED]->FreeList.end();it++)
            {
                if((*it)->length >= NormalWaitQueue.front()->memsize)
                {
                    tid = NormalWaitQueue.front()->Thread_ID;
                    NormalWaitQueue.erase(NormalWaitQueue.begin());
                    ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_READY;
                    PlaceIntoQueue(tid);
                    //cerr<<endl<<"exit wait queue"<<tid<<endl;
                }
            }
        }
        else if(!LowWaitQueue.empty())
        {
            for(it = MemoryIDVector[VM_MEMORY_POOL_ID_SHARED]->FreeList.begin(); it != MemoryIDVector[VM_MEMORY_POOL_ID_SHARED]->FreeList.end();it++)
            {
                if((*it)->length >= LowWaitQueue.front()->memsize)
                {
                    tid = LowWaitQueue.front()->Thread_ID;
                    LowWaitQueue.erase(LowWaitQueue.begin());
                    ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_READY;
                    PlaceIntoQueue(tid);
                    //cerr<<endl<<"exit wait queue"<<tid<<endl;
                }
            }
        }
        
    }

    void scheduler()
    {
        TVMThreadID tid;
        TVMThreadID Original = CurrentThreadIndex;
        //cerr<<"called sched "<< CurrentThreadIndex<< endl;
        if(ThreadIDVector[CurrentThreadIndex]->ThreadState == VM_THREAD_STATE_RUNNING)
        {
            //cerr<<"in here"<<endl;
            if(!HighQueue.empty()&&(VM_THREAD_PRIORITY_HIGH > ThreadIDVector[CurrentThreadIndex]->ThreadPriority))
            {
                tid = HighQueue.front()->Thread_ID;
                HighQueue.erase(HighQueue.begin());
                ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_RUNNING;
                CurrentThreadIndex = tid;
                ThreadIDVector[Original]->ThreadState = VM_THREAD_STATE_READY;
                PlaceIntoQueue(Original);

                MachineContextSwitch(&ThreadIDVector[Original]->context,&ThreadIDVector[tid]->context);

            }
            else if(!NormalQueue.empty()&&(VM_THREAD_PRIORITY_NORMAL > ThreadIDVector[CurrentThreadIndex]->ThreadPriority))
            {
                tid = NormalQueue.front()->Thread_ID;
                NormalQueue.erase(NormalQueue.begin());
                ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_RUNNING;
                CurrentThreadIndex = tid;
                ThreadIDVector[Original]->ThreadState = VM_THREAD_STATE_READY;
                PlaceIntoQueue(Original);
                //cerr<<endl<<"switch to "<< CurrentThreadIndex<<endl;
                MachineContextSwitch(&(ThreadIDVector[Original]->context),&(ThreadIDVector[tid]->context));

            }

            else if(!LowQueue.empty()&&(VM_THREAD_PRIORITY_LOW > ThreadIDVector[CurrentThreadIndex]->ThreadPriority))
            {
                tid = LowQueue.front()->Thread_ID;
                LowQueue.erase(LowQueue.begin());
                ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_RUNNING;
                CurrentThreadIndex = tid;
                ThreadIDVector[Original]->ThreadState = VM_THREAD_STATE_READY;
                PlaceIntoQueue(Original);
                MachineContextSwitch(&(ThreadIDVector[Original]->context),&(ThreadIDVector[tid]->context));

            }
            /* for idle
            else
            {
                CurrentThreadIndex = 1;
                ThreadIDVector[1]->ThreadState = VM_THREAD_STATE_RUNNING;
                MachineContextSwitch(ThreadIDVector[Original]->context, ThreadIDVector[1]->context);
            }*/
        }    

        //current thread is not running, so just find the highest priority thread
        else
        {
            if(!HighQueue.empty())
            {
                tid = HighQueue.front()->Thread_ID;
                HighQueue.erase(HighQueue.begin());
                ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_RUNNING;
                CurrentThreadIndex = tid;
                MachineContextSwitch(&ThreadIDVector[Original]->context,&ThreadIDVector[tid]->context);

            }
            else if(!NormalQueue.empty())
            {
                tid = NormalQueue.front()->Thread_ID;
                NormalQueue.erase(NormalQueue.begin());
                ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_RUNNING;
                CurrentThreadIndex = tid;
                //cerr<<endl<<"switch to "<< CurrentThreadIndex<<endl;
                MachineContextSwitch(&ThreadIDVector[Original]->context,&ThreadIDVector[tid]->context);

            }

            else if(!LowQueue.empty())
            {
                tid = LowQueue.front()->Thread_ID;
                LowQueue.erase(LowQueue.begin());
                ThreadIDVector[tid]->ThreadState = VM_THREAD_STATE_RUNNING;
                CurrentThreadIndex = tid;
                MachineContextSwitch(&ThreadIDVector[Original]->context,&ThreadIDVector[tid]->context);

            }

            else
            {
                CurrentThreadIndex = 1;
                ThreadIDVector[1]->ThreadState = VM_THREAD_STATE_RUNNING;
                //cerr<<"go to idle"<<endl;
                MachineContextSwitch(&ThreadIDVector[Original]->context, &ThreadIDVector[1]->context);
            }
        }
    }

    //used to sort the block address
    bool compareBlockAddresses(const block* block1, const block* block2)
    {
        return ( block1->address < block2->address);
    }

    //initalize everything from out memorypool struct and block struck
    //block is inside out memory pool stuct so we initalize it here too
    TVMStatus VMMemoryPoolCreate(void* base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        if(base == NULL||memory == NULL|| size == 0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        *memory = MemoryIDVector.size();
        MemoryIDVector.push_back(new MemoryPool);

        MemoryIDVector[*memory]->PoolID = *memory;
        MemoryIDVector[*memory]->base = (uint8_t*)base;
        MemoryIDVector[*memory]->MemoryPoolSize = size;
        //other stuff for later
        block *ablock = new block;
        ablock->length = size;
        ablock->address = (uint8_t*)base;
        MemoryIDVector[*memory]->FreeList.push_back(ablock);
        MemoryIDVector[*memory]->FreeSpace = size;

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        if(memory < 0 || memory >= MemoryIDVector.size() || size == 0 || pointer == NULL)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

       /* if(MemoryIDVector[memory]->FreeSpace < size)
        {
            return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
        }*/
        //round up to nearest multiple of 64
        if((size % 64) > 0)
        {
            size = (size+64)/64*64;
        }
        //check first block with enough space,  set the new base, place into allocate
        list<block*>::iterator it;
        //from beginning of freelist to end
        for(it = MemoryIDVector[memory]->FreeList.begin(); it != MemoryIDVector[memory]->FreeList.end(); it++)
        {
            //If the free space block has enough size then allocate
            if((*it)->length >= size)
            {
                block* aBlock = new block;
                aBlock->address = (*it)->address;
                aBlock->length = size;
                *pointer = (*it)->address;
                MemoryIDVector[memory]->AllocatedList.push_back(aBlock);
                
                //used to set new base in the block
                //If size != length, cut the block 
                if(size != (*it)->length)
                {
                    //reduce the length
                    (*it)->length -= size;
                    //new base
                    (*it)->address += size;

                }
                //if the allocated size is equal to the length of the block's free space
                //we need to just erase the block fromt he freelist
                else
                {
                    it = MemoryIDVector[memory]->FreeList.erase(it);
                }

                MemoryIDVector[memory]->FreeSpace -= size;
                MemoryIDVector[memory]->FreeList.sort(compareBlockAddresses);
                MachineResumeSignals(&OldState);
                return VM_STATUS_SUCCESS;
            }//if enough space
        }

        MachineResumeSignals(&OldState);
        return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
    }

    //merge only if it is contiguous
    //iterate through the memory pool's free list and merge any possible blocks
    void MergeFreeBlocks(TVMMemoryPoolID memory)
    {
        list<block*>::iterator it1;
        list<block*>::iterator it2 = MemoryIDVector[memory]->FreeList.begin();
        it2++;

        for(it1 = MemoryIDVector[memory]->FreeList.begin(); it2 != MemoryIDVector[memory]->FreeList.end();it1++,it2++)
        {
            //if the address of the next block is equal to the address of current block + length then it is continuous
            if(((*it1)->address+(*it1)->length) ==((*it2)->address))
            {
                (*it1)->length += (*(it2))->length;
                it2 = MemoryIDVector[memory]->FreeList.erase(it2);
                it1--;
                it2--; 
            }
        }
    }

    TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);
        if(memory < 0 || memory >= MemoryIDVector.size())
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        list<block*>::iterator it;
        //search the allocated list for the block we want to deallocate
        for(it = MemoryIDVector[memory]->AllocatedList.begin(); it != MemoryIDVector[memory]->AllocatedList.end(); it++)
        {
            if((*it)->address == pointer)
            {
                //adds length of freespace remove from allocatedlist remove from freelist sort then merge 
                //remove from allocated, place into free, sort, iterate through free and merge
                block* aBlock = new block;
                aBlock->address = (*it)->address;
                aBlock->length = (*it)->length;
                MemoryIDVector[memory]->FreeSpace += (*it)->length;
                MemoryIDVector[memory]->FreeList.push_back(aBlock);
                MemoryIDVector[memory]->FreeList.sort(compareBlockAddresses);
                MemoryIDVector[memory]->AllocatedList.erase(it);
                MergeFreeBlocks(memory);

                break;
            }
        }

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
    }
    
    TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        if(memory < 0 || memory >= MemoryIDVector.size())
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        MemoryIDVector.erase(MemoryIDVector.begin()+memory);

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        if(memory < 0 || memory >= MemoryIDVector.size() || bytesleft == NULL)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        //bytesleft equals the addess of freespace
        *bytesleft = MemoryIDVector[memory]->FreeSpace;

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
    }

    void AlarmRequestCallback(void *var)
    {
        for(unsigned  i = 0; i < SleepingQueue.size(); i++)
        {
            SleepingQueue[i]->ticks--;
            //If ticks are at zero remove it from the sleeping queue and place into ready queue
            if(SleepingQueue[i]->ticks == 0)
            {
                SleepingQueue[i]->ThreadState = VM_THREAD_STATE_READY;
                PlaceIntoQueue(SleepingQueue[i]->Thread_ID);
                SleepingQueue.erase(SleepingQueue.begin() + i);
                i--;
            }
        }

      
        //if current thread is running then put it into a ready queue
        if(ThreadIDVector[CurrentThreadIndex]->ThreadState == VM_THREAD_STATE_RUNNING)
        {
            ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_READY;
            PlaceIntoQueue(CurrentThreadIndex);
        }       
        scheduler();
    }



    TVMStatus VMStart(int tickms, TVMMemorySize heapsize, int machinetickms, TVMMemorySize sharedsize, const char *mount, int argc, char *argv[])
	{    
        //declare it
		TVMMainEntry VMMain;
        //load the module
		VMMain = VMLoadModule(argv[0]);	
        uint8_t* FileImageData;
        TVMMemoryPoolID id  = 12323;

        TVMMemorySize altsharedsize = sharedsize;
        //mounting 
        MachineFileOpen(mount, O_RDWR, 0644, FileCallback, ThreadIDVector[CurrentThreadIndex]);
        ThreadIDVector[CurrentThreadIndex]->ThreadState=VM_THREAD_STATE_WAITING;

        scheduler();
        VMMemoryPoolCreate(FileImageData, 512, &id);
        VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM, 512, (void**)&(ThreadIDVector[VM_MEMORY_POOL_ID_SYSTEM]->BaseStack));
        MachineFileRead(3, data, length, FileCallback, calldata);
       
        //round up to nearest multiple of 4096
        //sharedmem size
        if((sharedsize % 4096) > 0)
        {
            altsharedsize = (sharedsize + 4096)/4096*4096;
        }

        void* sharedBase = MachineInitialize(machinetickms, altsharedsize);
        MachineRequestAlarm(tickms*1000,(TMachineAlarmCallback)AlarmRequestCallback,NULL);

        uint8_t* aBase = new uint8_t[heapsize];

        
        //regular memory
        VMMemoryPoolCreate(aBase, heapsize, &id);

        //shared memory
        VMMemoryPoolCreate((uint8_t*)sharedBase, altsharedsize, &id);

        //create the system memory pool
        //create main thread
        ThreadIDVector.push_back(new TCB);
        ThreadIDVector[0]->Thread_ID = 0;
        ThreadIDVector[0]->ThreadPriority = VM_THREAD_PRIORITY_NORMAL;
        ThreadIDVector[0]->ThreadState = VM_THREAD_STATE_RUNNING;
        
        CurrentThreadIndex = 0;

        ThreadIDVector.push_back(new TCB);
        ThreadIDVector[1]->entry = IdleEntry;
        ThreadIDVector[1]->Thread_ID = 1;
        ThreadIDVector[1]->ThreadState = VM_THREAD_STATE_READY;
        ThreadIDVector[1]->ThreadPriority = 0x00;
        ThreadIDVector[1]->MemorySize = 0x100000;
        VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM,ThreadIDVector[1]->MemorySize, (void**)&(ThreadIDVector[1]->BaseStack));

        MachineContextCreate(&(ThreadIDVector[1]->context), IdleEntry , NULL,ThreadIDVector[1]->BaseStack, ThreadIDVector[1]->MemorySize);


        //if valid address
        if(VMMain != NULL)
        {
            MachineEnableSignals();
            VMMain(argc, argv);
            return VM_STATUS_SUCCESS;
        }
        else
        {
            return VM_STATUS_FAILURE;
        }

	}

    TVMStatus VMThreadSleep(TVMTick tick)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        if(tick == VM_TIMEOUT_INFINITE)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

		if(tick == VM_TIMEOUT_IMMEDIATE)
		{
			ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_READY;
			PlaceIntoQueue(CurrentThreadIndex);
			scheduler();
		}
        //add if VM_TIMEOUT_ERROR
        ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;

        ThreadIDVector[CurrentThreadIndex]->ticks = tick;

        SleepingQueue.push_back(ThreadIDVector[CurrentThreadIndex]);

        scheduler();

        MachineResumeSignals(&OldState);

        return VM_STATUS_SUCCESS;
		
    }

    void FileCallback(void* calldata, int result)
    {    
        TCB* MyTCB = (TCB*)calldata;
        //ThreadIDVector[MyTCB->Thread_ID]->file = result;
        MyTCB->file = result;
        MyTCB->ThreadState = VM_THREAD_STATE_READY;
        //cerr<<endl<<"callback "<< MyTCB->Thread_ID <<endl;

        PlaceIntoQueue(MyTCB->Thread_ID);
        scheduler();
    }

    TVMStatus VMFileWrite(int filedescriptor, void *data, int *length)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);
        //cerr<<endl<<"enter write "<<CurrentThreadIndex<<endl;
        if(data == NULL || length == NULL)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        int LengthRemaining = *length;
        int CurrentLength;
        int it = 0;
       // char* FullString = (char *)data;

        ThreadIDVector[CurrentThreadIndex]->memsize = *length;

        void *shared = NULL;      
      /*  
        if(*length > 512)
        {
            while(VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SHARED, 512, &shared) == VM_STATUS_ERROR_INSUFFICIENT_RESOURCES)
            {
                ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
                PlaceIntoWaitQueue(CurrentThreadIndex);
                scheduler();
            }    
        }
        else
        {
            while(VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SHARED, *length, &shared) == VM_STATUS_ERROR_INSUFFICIENT_RESOURCES)
            {
                ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
                PlaceIntoWaitQueue(CurrentThreadIndex);
                scheduler();
            }
        }
        //after while loop check if enough space for next thing to run
        CheckForFreeSharedSpace();
        scheduler();
*/
        while(VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SHARED, 512, &shared) == VM_STATUS_ERROR_INSUFFICIENT_RESOURCES)
        {
            ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
            PlaceIntoWaitQueue(CurrentThreadIndex);
            scheduler();
        }    
        //while loop to make sure data transfer is in 512 byte segments
        while(LengthRemaining > 0)
        {
            if(LengthRemaining > 512)
            {
                LengthRemaining -= 512;
                CurrentLength = 512;
            }
            else
            {
                CurrentLength = LengthRemaining;
                LengthRemaining = 0;
            }
           /* char tempString[CurrentLength];

            for(int i = 0; i < CurrentLength; i++, it++)
            {
                tempString[i] = FullString[it];
            }*/

           /* while(VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SHARED, CurrentLength, &shared) == VM_STATUS_ERROR_INSUFFICIENT_RESOURCES)
            {
                ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
                PlaceIntoWaitQueue(CurrentThreadIndex);
                scheduler();
            }  */  

            memcpy(shared,(char*)data + it,CurrentLength);
            it+=CurrentLength;
            MachineFileWrite(filedescriptor, shared, CurrentLength, FileCallback, ThreadIDVector[CurrentThreadIndex]);
            //cerr<<endl<<"CURRENT THREAD WAITING FOR FILEWRITE "<<CurrentThreadIndex<<endl;
            ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
            scheduler();  
            //cerr<<endl<<"CURRENT THREAD BACK "<<CurrentThreadIndex<<endl;
        }
        VMMemoryPoolDeallocate(VM_MEMORY_POOL_ID_SHARED, shared);       

        WaitToReady();
        //ThreadIDVector[CurrentThreadIndex]->ThreadState=VM_THREAD_STATE_READY;
        //PlaceIntoQueue(CurrentThreadIndex);
        //CheckForFreeSharedSpace();
        scheduler();
        
        if(ThreadIDVector[CurrentThreadIndex]->file < 0)
        {
            //cerr<<endl<<"exit write fail "<<CurrentThreadIndex<<endl;
            MachineResumeSignals(&OldState);
            return VM_STATUS_FAILURE;
        }
        //cerr<<endl<<"exit write success"<< CurrentThreadIndex<< endl;
        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;      
    }    

    TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);
        if(filename == NULL || filedescriptor == NULL)
        {
            //return error
			MachineResumeSignals(&OldState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        MachineFileOpen(filename, flags, mode, (TMachineFileCallback)FileCallback, ThreadIDVector[CurrentThreadIndex]);

		//return vmstatusfailture if fileopen cant open
		if(ThreadIDVector[CurrentThreadIndex]->file < 0)
		{
			MachineResumeSignals(&OldState);
			return VM_STATUS_FAILURE;
		}

        ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
        //WaitingQueue.push_back(ThreadIDVector[CurrentThreadIndex]);
        scheduler();

        *filedescriptor = ThreadIDVector[CurrentThreadIndex]->file;
        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
		
		
    }

    TVMStatus VMFileClose(int filedescriptor)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);


        MachineFileClose(filedescriptor, FileCallback, ThreadIDVector[CurrentThreadIndex]);
        ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
        scheduler();  

        if(ThreadIDVector[CurrentThreadIndex]->file < 0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_FAILURE;
        }

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;      

    }    

    TVMStatus VMFileRead(int filedescriptor, void *data, int *length)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        if(data == NULL || length == NULL)
        {
			MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        int temp = *length;
        int temp2;
        *length = 0;

        //error check to see if length is a valid value
        while(temp > 0)
        {
            //if temp is bigger then 512 bytes then set new temp to 512, the max it can be
            if(temp > 512)
            {
                temp -= 512;
                temp2 = 512;
            }
            //set newtemp to temp's value to keep error checking temp
            else
            {
                temp2 = temp;
                temp = 0;
            }

            void *shared; 
            //gets it from shared memory
            VMMemoryPoolAllocate(1, (TVMMemorySize)temp2, &shared);

            MachineFileRead(filedescriptor, shared, temp2, FileCallback, ThreadIDVector[CurrentThreadIndex]);
            ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
            scheduler();  

            //data=destination, shared=source, temp2=size
            memcpy(data, shared, temp2);
            VMMemoryPoolDeallocate(VM_MEMORY_POOL_ID_SHARED, shared);

            *length += ThreadIDVector[CurrentThreadIndex]->file;
        }//while(temp>0)

        if(ThreadIDVector[CurrentThreadIndex]->file < 0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_FAILURE;
        }
        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;      

    }    


    TVMStatus VMFileSeek(int filedescriptor, int offset, int whence,int *newoffset)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        MachineFileSeek(filedescriptor, offset, whence, FileCallback, ThreadIDVector[CurrentThreadIndex]);
        ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
        scheduler();  

        *newoffset = ThreadIDVector[CurrentThreadIndex]->file;
        if(ThreadIDVector[CurrentThreadIndex]->file < 0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_FAILURE;
        }
        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;      

    }    

    TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        *tid = ThreadIDVector.size();
        ThreadIDVector.push_back(new TCB);
        //thread id is equal to the size of the vector so it can be added to the end
        ThreadIDVector[*tid]->Thread_ID = *tid;
        ThreadIDVector[*tid]->entry = entry;
        ThreadIDVector[*tid]->ThreadParameter = param;
        ThreadIDVector[*tid]->MemorySize = memsize;
        ThreadIDVector[*tid]->ThreadPriority = prio;
        ThreadIDVector[*tid]->ThreadState = VM_THREAD_STATE_DEAD;
        VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM, memsize, (void**)&(ThreadIDVector[*tid]->BaseStack));

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMThreadActivate(TVMThreadID thread)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        MachineContextCreate(&(ThreadIDVector[thread]->context), SkeletonFunction, ThreadIDVector[thread]->ThreadParameter,ThreadIDVector[thread]->BaseStack, ThreadIDVector[thread]->MemorySize);

        ThreadIDVector[thread]->ThreadState = VM_THREAD_STATE_READY;

        //switch statement to determine which priority queue to place thread into
        PlaceIntoQueue(thread);

        scheduler();

        MachineResumeSignals(&OldState);

        return VM_STATUS_SUCCESS;
    }

    

    TVMStatus VMThreadTerminate(TVMThreadID thread)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        if(thread >= ThreadIDVector.size() || thread < 0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_ID;
        }
        if(ThreadIDVector[thread]->ThreadState == VM_THREAD_STATE_DEAD)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_STATE;
        }

        ThreadIDVector[thread]->ThreadState = VM_THREAD_STATE_DEAD;

        scheduler();
        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;

    }

    TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        if(thread >= ThreadIDVector.size()|| thread<0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_ID;
        }

        if(stateref == NULL)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        *stateref = ThreadIDVector[thread]->ThreadState;
     
        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;

    }


    TVMStatus VMThreadDelete(TVMThreadID thread)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);


        //if out of bounds or thread is deleted
        if(thread > (ThreadIDVector.size()-1) || thread < 0 ||ThreadIDVector[thread] == NULL)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_ID;
        }
        //if state is dead delete the thread
        if(ThreadIDVector[thread]->ThreadState == VM_THREAD_STATE_DEAD)
        {
            ThreadIDVector[thread] = NULL;
            MachineResumeSignals(&OldState);
            return VM_STATUS_SUCCESS;
        }
        //state is not dead
        else
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_STATE;
        }

    }

    TVMStatus VMThreadID(TVMThreadIDRef threadref)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        if(threadref == NULL)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        *threadref =  ThreadIDVector[CurrentThreadIndex]->Thread_ID;
        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;


    }



	TVMStatus VMMutexCreate(TVMMutexIDRef mutexref)
	{
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

		if(mutexref == NULL)
		{
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

    	*mutexref =  MutexIDVector.size();



		MutexIDVector.push_back(new mutex);
		
		MutexIDVector[*mutexref]->MutexID = MutexIDVector.size()-1;
        MutexIDVector[*mutexref]->unlocked = true;

        MachineResumeSignals(&OldState);
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout)
	{
	    TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);
       
        if(mutex >= MutexIDVector.size()||mutex < 0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_ID;
        }

        if(timeout == VM_TIMEOUT_IMMEDIATE)
        {
            if(MutexIDVector[mutex]->OwnerID < 0)
            {
                MutexIDVector[mutex]->OwnerID = CurrentThreadIndex;
                MachineResumeSignals(&OldState);
                return VM_STATUS_SUCCESS;
              // ThreadIDVector[CurrentThreadIndex]->ThreadState =VM_THREAD_STATE_READY;
                //PlaceIntoQueue(CurrentThreadIndex);
                //scheduler();
            }
            else
            {
                MachineResumeSignals(&OldState);
                return VM_STATUS_FAILURE;
            }
        }
        //mutex available
        if(MutexIDVector[mutex]->unlocked)
        {
            MutexIDVector[mutex]->OwnerID = CurrentThreadIndex;
            MutexIDVector[mutex]->unlocked =false;
        }
        //mutex unavailable place in waiting
        else
        {
            ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
            ThreadIDVector[CurrentThreadIndex]->ticks = timeout;
            SleepingQueue.push_back(ThreadIDVector[CurrentThreadIndex]);
            PlaceIntoMutexQueue(CurrentThreadIndex,mutex);

            scheduler();
        

           /* if(ThreadIDVector[CurrentThreadIndex]->ticks == 0)
            {    
                switch(ThreadIDVector[CurrentThreadIndex]->ThreadPriority)
                {
                    case VM_THREAD_PRIORITY_LOW:
                        MutexIDVector[mutex]->LowPrio.erase(MutexIDVector[mutex]->LowPrio.begin()+ThreadIDVector[CurrentThreadIndex]->MutexPrioIndex);
                        break;
                    case VM_THREAD_PRIORITY_NORMAL:
                        MutexIDVector[mutex]->NormalPrio.erase(MutexIDVector[mutex]->NormalPrio.begin()+ThreadIDVector[CurrentThreadIndex]->MutexPrioIndex);
                        break;
                    case VM_THREAD_PRIORITY_HIGH:
                        MutexIDVector[mutex]->HighPrio.erase(MutexIDVector[mutex]->HighPrio.begin()+ThreadIDVector[CurrentThreadIndex]->MutexPrioIndex);
                        break;
                }

                MachineResumeSignals(&OldState);
                return VM_STATUS_FAILURE;
            }*/
        }

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;	
	}

    TVMStatus VMMutexRelease(TVMMutexID mutex)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

		if(mutex >= MutexIDVector.size()||mutex < 0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_ID;
        }

        if(!MutexIDVector[mutex]->HighPrio.empty())
        {
            MutexIDVector[mutex]->OwnerID = MutexIDVector[mutex]->HighPrio.front()->Thread_ID;
            MutexIDVector[mutex]->HighPrio.erase(MutexIDVector[mutex]->HighPrio.begin());
            ThreadIDVector[MutexIDVector[mutex]->OwnerID]->ThreadState = VM_THREAD_STATE_READY;
            for(unsigned i = 0; i < SleepingQueue.size(); i++)
            {
                if(SleepingQueue[i]->Thread_ID == MutexIDVector[mutex]->OwnerID)
                {
                    SleepingQueue.erase(SleepingQueue.begin()+i);
                    break;
                }
            }
            PlaceIntoQueue(MutexIDVector[mutex]->OwnerID);

            scheduler();
        }
        else if(!MutexIDVector[mutex]->NormalPrio.empty())
        {
            MutexIDVector[mutex]->OwnerID = MutexIDVector[mutex]->NormalPrio.front()->Thread_ID;
            MutexIDVector[mutex]->NormalPrio.erase(MutexIDVector[mutex]->NormalPrio.begin());
            ThreadIDVector[MutexIDVector[mutex]->OwnerID]->ThreadState = VM_THREAD_STATE_READY;
            for(unsigned i = 0; i < SleepingQueue.size(); i++)
            {
                if(SleepingQueue[i]->Thread_ID == MutexIDVector[mutex]->OwnerID)
                {
                    SleepingQueue.erase(SleepingQueue.begin()+i);
                    break;
                }
            }
            PlaceIntoQueue(MutexIDVector[mutex]->OwnerID);
            scheduler();
        }
        else if(!MutexIDVector[mutex]->LowPrio.empty())
        {
            MutexIDVector[mutex]->OwnerID = MutexIDVector[mutex]->LowPrio.front()->Thread_ID;
            MutexIDVector[mutex]->LowPrio.erase(MutexIDVector[mutex]->LowPrio.begin());
            ThreadIDVector[MutexIDVector[mutex]->OwnerID]->ThreadState = VM_THREAD_STATE_READY;
            for(unsigned i = 0; i < SleepingQueue.size(); i++)
            {
                if(SleepingQueue[i]->Thread_ID == MutexIDVector[mutex]->OwnerID)
                {
                    SleepingQueue.erase(SleepingQueue.begin()+i);
                    break;
                }
            }
            PlaceIntoQueue(MutexIDVector[mutex]->OwnerID);
            scheduler();
        }

        else
        {
            MutexIDVector[mutex]->unlocked = true;
            scheduler();
        }

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
		
    }

    TVMStatus VMMutexDelete(TVMMutexID mutex)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        if(mutex < 0|| mutex > MutexIDVector.size()-1)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_ID;
        }

        if(MutexIDVector[mutex]->OwnerID < 0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_STATE;
        }

        MutexIDVector.erase(MutexIDVector.begin() + mutex);

		MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref)
    {
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);
        
        if(mutex < 0|| mutex > MutexIDVector.size()-1)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_ID;
        }
		if(MutexIDVector[mutex]->unlocked)
		{
			MachineResumeSignals(&OldState);
			return VM_THREAD_ID_INVALID;
		}
        if(ownerref == NULL)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        *ownerref = MutexIDVector[mutex]->OwnerID;

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
    }
/*
    TVMStatus VMDirectoryOpen(const char *dirname, int *dirdescriptor)
    {
        MachineSuspendSignals OldState;
        MachineResumeSignals(&OldState);
        if(dirname == NULL || dirdescriptor == NULL)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        if(ThreadIDVector[CurrentThreadIndex]-> < 0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_FAILURE;
        }

        MachineFileOpen(dirname, O_RDWR, mode, FileCallback, ThreadIDVector[CurrentThreadIndex]);

        ThreadIDVector[CurrentThreadIndex]->ThreadState = VM_THREAD_STATE_WAITING;
        
        scheduler();

        *dirdescriptor = ThreadIDVector[CurrentThreadIndex]->;
        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
    }
    TVMStatus VMDirectoryClose(int dirdescriptor)
    {
        MachineSuspendSignals OldState;
        MachineResumeSignals(&OldState);

        if(ThreadIDVector[CurrentThreadIndex]->< 0)
        {
            MachineResumeSignals(&OldState);
            return VM_STATUS_FAILURE;
        }
    }*/
}
