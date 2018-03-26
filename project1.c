#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string.h>
#include <semaphore.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <strings.h>
#include <sys/stat.h>
#include <ctype.h>

#define N 10                    //N blocks
#define SHM_NAME "/shmem"               //naming the shared memory

struct block{
        int  id;                //block index in storage
        int data;               //data in block
};

//List of size N, represented as array
struct list{
        char name[10];
        int data[N];            //indexes of block
        int count;              //number of elements in list, used only to return last block
        sem_t b_mutex;          //binary semaphore
        sem_t c_sem;    //counting semaphore for available blocks in list.
};

//storage shared between processes, which is a structure
enum LIST_NAME { LIST1=0, LIST2};
struct memoryStorage{
        struct block     blocks[N];
        struct list flist;      //Freelist
        struct list lists[2];   //List1 and List2
        sem_t mem_sem;          //semaphore used for accessing memory
};

static int shmid = -1;
static struct memoryStorage * mem = NULL;
static pid_t pid[3];                            //child PIDs
static char Pname[2] = "Px";                    //name of process
//Initializing the shared storage
static int create_memory(struct memoryStorage *storage){
        int i,j;

        //initializing storage blocks
        for(i=0; i < N; i++){
                mem->blocks[i].id = i;
                mem->blocks[i].data = 0;
        }

        //Initializing list1 and list2
        struct list * list;
        for(i=0; i < 2; i++){
                list = &storage->lists[i];
                for(j=0; j < N; j++){
                        list->data[j] = -1;     //If index of block is invalid, marks list node as empty
                }
                list->count = 0;                //list is empty

                if(     (sem_init(&list->c_sem, 1, 0) == -1) ||         //counting semaphore
                        (sem_init(&list->b_mutex,  1, 1) == -1) ){      //binary semaphore initialized to 1
                        perror("sem_init");
                        return 1;
                }
                snprintf(list->name, 10, "List%i", i+1);
        }

        //initialize Freelist separately, since its full at start
        list = &storage->flist;
        if(     (sem_init(&list->c_sem, 1, N-1) == -1) ||       //initializing the counting semaphore on freelist to N-1
                (sem_init(&list->b_mutex,  1, 1) == -1) ){      //initializing binary semaphore
                perror("sem_init");
                return 1;
        }

        //insert free blocks indexes in freelist
        for(i=0; i < N; i++){
                list->data[i] = i;
        }
        list->count = N;        //we have N free blocks
        strncpy(list->name, "FreeList", 10);
   if(     sem_init(&storage->mem_sem,  1, 1) == -1        ){      //binary sem
                perror("sem_init");
                return 1;
        }

        return 0;
}

static void delete_memory(struct memoryStorage *storage){
        int i;

        //destroy all list semaphores
        for(i=0; i < 2; i++){
                struct list * list = &storage->lists[i];
                if((sem_destroy(&list->b_mutex) == -1)  ||
                (sem_destroy(&list->c_sem) == -1)){
                        perror("sem_destroy");
                }
        }

        if((sem_destroy(&storage->flist.b_mutex) == -1)||
        (sem_destroy(&storage->flist.c_sem) == -1)){
                perror("sem_destroy");
        }
}
// initialize shared memory
static int init_shm(const int is_child){

        //create shared environment
        shm_unlink(SHM_NAME);   //unlink shm, if program crashed
        shmid = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
        if(shmid == -1){
                perror("shm_open");
                return 1;
        }

        //resize the shared region to take on storage structure
        if(ftruncate(shmid, sizeof(struct memoryStorage)) == -1){
                perror("ftruncate");
                return 1;
        }
 //map it to our memory space, which is inherited by all children
        mem = (struct memoryStorage *) mmap(NULL, sizeof(struct memoryStorage), PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
        if(mem == (void*) -1){
                perror("mmap");
                return 1;//error
        }
        return 0;       //success
}

//cleanup the shared memory
static void delete_shm(){
        munmap(mem, sizeof(struct memoryStorage));
        shm_unlink(SHM_NAME);
}

//to print the list.
static void print_list(struct list * list, const char * msg){
        int i;

        printf("%s %s[", Pname, list->name);
        for(i=0; i < list->count; i++){
                printf("%i ", list->data[i]);   //show block index
        }
        printf("] %s\n", msg);

        fflush(stdout);
}

//unlink operation on lists 
struct block* unLink(struct list *list){
        int i,bi;       //block index

        print_list(list, "before unlink");
                bi = list->data[0];     //take first free block index
                //shift other blocks left
                for(i=1; i < list->count; i++){
                        list->data[i-1] = list->data[i];
                }
                list->count--;
        print_list(list, "after unlink");
 return &mem->blocks[bi];        //return block pointer
}

//link operation on lists
void Link(struct block *b, struct list * list){

        print_list(list, "before link");
                list->data[list->count] = b->id;        //push block index at end of list
                list->count++;
        print_list(list, "after link");
}

//Production of information in blocks
void produce_information_in_block(struct block *b){
        static int B = 0;       //block counter

        b->data = B++;  //produce information in block b
}

//p2 uses this function to produce information into y
void use_block_x_to_produce_info_in_y(struct block *b, struct block *y){
        y->data = b->data * -1;
}

void consume_information_in_block(struct block *c){
        //printf("B[%i]=%i\n", c->id, c->data);
}

//helper functions to wait and post on a semaphore
#define WAIT(x) sem_wait(x)
#define POST(x) sem_post(x)

//execution starts here
int main(void){

        if(init_shm(0) == 1){   //exits(1) on failure
                return 1;
        }

        //initialize shared environment
        if(create_memory(mem) == 1){
                delete_shm();
 fprintf(stderr, "Error: Failed to initialize storage\n");
                return 1;
        }

        //create 3 processes
// process1
        pid[0] = fork();
        if(pid[0] == 0){
        Pname[1] = '1';
        struct block * b;
        while(1){
                WAIT(&mem->flist.c_sem);                        //wait on counting semaphore of list1
                WAIT(&mem->flist.b_mutex);                      //wait on binary semaphore of list1
                        b = unLink(&mem->flist);                //unlinking a block from freelist
                POST(&mem->flist.b_mutex);		
                        produce_information_in_block(b);                //producing information into block b
                WAIT(&mem->lists[LIST1].b_mutex);
                        Link(b, &mem->lists[LIST1]);            //linking block b to list1
                POST(&mem->lists[LIST1].c_sem);
                POST(&mem->lists[LIST1].b_mutex);
        }
        exit(0);
        }
        else if(pid[0]  < 0){
        perror("fork");
        }

//process2
        pid[1] = fork();
        if(pid[1] == 0){
        Pname[1] = '2';
        struct block *x,*y;
        while(1){
                WAIT(&mem->lists[LIST1].c_sem);			//wait on counting semaphore of list1
                WAIT(&mem->lists[LIST1].b_mutex);		//wait on binary semaphore of list1
                        x = unLink(&mem->lists[LIST1]);		//unlink a block from list1
                POST(&mem->lists[LIST1].b_mutex);

                WAIT(&mem->mem_sem);
                WAIT(&mem->flist.b_mutex);
                        y = unLink(&mem->flist);		//unlink a block from freelist
                POST(&mem->flist.b_mutex);
	  use_block_x_to_produce_info_in_y(x,y);          //producing y using block x

                WAIT(&mem->flist.b_mutex);
                        Link(x, &mem->flist);                   //linking x to freelist
                POST(&mem->flist.c_sem);
                POST(&mem->flist.b_mutex);

                WAIT(&mem->lists[LIST2].b_mutex);
                        Link(y, &mem->lists[LIST2]);            //linking y to free list
                POST(&mem->lists[LIST2].c_sem);
                POST(&mem->lists[LIST2].b_mutex);
        }
        exit(0);
        }
        else if(pid[1]  < 0){
        perror("fork");
        }
//process3
        pid[2] = fork();
        if(pid[2] == 0){
        Pname[1] = '3';
        struct block *c;
        while(1){
                WAIT(&mem->lists[LIST2].c_sem);			//wait on counting semaphore of list2
                WAIT(&mem->lists[LIST2].b_mutex);		//wait on binary semaphore of list2
                        c = unLink(&mem->lists[LIST2]);		//unlink a block from free list
                POST(&mem->lists[LIST2].b_mutex);
                        consume_information_in_block(c);		//consuming information in block c
                WAIT(&mem->flist.b_mutex);
                        Link(c, &mem->flist);			//linking c to freelist
                POST(&mem->mem_sem);
                POST(&mem->flist.b_mutex);
        }
        exit(0);
        }
        else if(pid[2]  < 0){
        perror("fork");
        }
        int i, status;
        for(i=0; i < 3; i++){
                waitpid(pid[i], &status, 0);
        }
        delete_memory(mem);
        delete_shm();                           //delete shared memory
        return 0;
}
