#include <stdlib.h>
#include "mq.h"
#include <pthread.h>
#include "sync.h"
#include "double_link.h"
#include "link_list.h"
#include <signal.h>
#include "thread.h"
#include "SysTime.h"

struct per_thread_struct
{
	list_node   next;
	struct double_link_node block;
	struct link_list *local_q;
	condition_t cond;
	//标记是否已经被添加到thread_mq中，当一个线程第一次执行mq_push的时候，会将此mq添加到thread_mq中，并设置此标记
	int8_t      is_associate;
	int8_t      block_sig;
	int8_t      recved;
};

struct mq
{
	uint32_t           push_size;
	pthread_key_t      t_key;
	mutex_t            mtx;
	struct double_link blocks;
	struct link_list  *share_list;
	struct link_list  *local_lists;
};


struct mq_thread
{
	struct double_link_node dl_node;
	pthread_t   thread_id;
};

struct mq_system
{
	pthread_key_t       t_key;
	mutex_t             mtx;
	struct double_link  threads;
	struct link_list   *thread_mqs;
	thread_t            heart_thread;//心跳线程
	uint32_t            ms;
};

static struct mq_system *g_mq_system;


struct thread_mq_element
{
	list_node next;
	mq_t _mq;
};

/*线程mq管理结构，每线程一个,当线程第一次对一个mq执行mq_push时，会将这个mq添加到
* mqs中.
*/
struct thread_associate_mqs
{
	list_node   next;
	struct link_list *mqs;
};




//向所有使用了mq_push的线程发送信号，使其执行mq_force_sync
static inline void send_signal_to_all_threads(struct mq_system *_mq_system)
{
	if(_mq_system)
	{
		mutex_lock(_mq_system->mtx);
		struct double_link_node *n = _mq_system->threads.head.next;
		while(n != &_mq_system->threads.tail)
		{
			struct mq_thread *tmp = (struct mq_thread *)n;
			if(0 != pthread_kill(tmp->thread_id,SIGUSR1))
			{
				n = n->next;
				double_link_remove((struct double_link_node *)tmp);
				free(tmp);
			}else
				n = n->next;
		}
		mutex_unlock(_mq_system->mtx);
	}
}

static inline void add_thread(struct mq_system *_mq_system,pthread_t threadid)
{
	if(_mq_system)
	{
		mutex_lock(_mq_system->mtx);
		struct double_link_node *n = _mq_system->threads.head.next;
		while(n != &_mq_system->threads.tail)
		{
			if(((struct mq_thread*)n)->thread_id == threadid)
			{
				mutex_unlock(_mq_system->mtx);
				return;
			}
			n = n->next;
		}
		struct mq_thread *_new = (struct mq_thread*)calloc(1,sizeof(*_new));
		_new->thread_id = threadid;
		double_link_push(&_mq_system->threads,(struct double_link_node*)_new);
		mutex_unlock(_mq_system->mtx);
	}
}

static void signal_handler(int sig);


void *heart_routine(void *arg)
{
	while(1)
	{
		send_signal_to_all_threads(g_mq_system);
		sleepms(g_mq_system->ms);
	}
	return NULL;
}

void init_mq_system(uint32_t ms)
{
	g_mq_system = (struct mq_system *)calloc(1,sizeof(*g_mq_system));
	g_mq_system->mtx = mutex_create();
	g_mq_system->thread_mqs = LINK_LIST_CREATE();
	g_mq_system->ms = ms;
	double_link_clear(&g_mq_system->threads);
	pthread_key_create(&g_mq_system->t_key,0);
    //初始化信号
    struct sigaction sigusr1;
    sigusr1.sa_flags = 0;
    sigusr1.sa_handler = signal_handler;
    sigemptyset(&sigusr1.sa_mask);
    int status = sigaction(SIGUSR1,&sigusr1,NULL);
    g_mq_system->heart_thread = create_thread(0);
	thread_start_run(g_mq_system->heart_thread,heart_routine,NULL);	
}


static struct per_thread_struct *per_thread_create()
{
	struct per_thread_struct *pts = calloc(1,sizeof(*pts));
	pts->local_q = LINK_LIST_CREATE();
	pts->cond = condition_create();
	return pts;
}

static void per_thread_destroy(struct per_thread_struct **pts)
{
	LINK_LIST_DESTROY(&(*pts)->local_q);
	condition_destroy(&(*pts)->cond);
	free(*pts);
	*pts = NULL;
}


mq_t create_mq(uint32_t push_size)
{
	mq_t m = calloc(1,sizeof(*m));
	m->mtx = mutex_create();
	pthread_key_create(&m->t_key,0);
	m->share_list = LINK_LIST_CREATE();
	m->local_lists = LINK_LIST_CREATE();
	double_link_clear(&m->blocks);
	m->push_size = push_size;
	return m; 	
}

void destroy_mq(mq_t *m)
{
	mutex_destroy(&(*m)->mtx);
	LINK_LIST_DESTROY(&(*m)->share_list);
	list_node *l = link_list_head((*m)->local_lists);
	while(l)
	{
		struct per_thread_struct *pts = (struct per_thread_struct*)l;
		l = l->next;
		per_thread_destroy(&pts);
	}	
	LINK_LIST_DESTROY(&(*m)->local_lists);
	free(*m);
	*m = NULL;		
}

static inline mq_sync_push(mq_t m,struct per_thread_struct *pts)
{
	mutex_lock(m->mtx);
	uint8_t empty = link_list_is_empty(m->share_list);
	link_list_swap(m->share_list,pts->local_q);
	if(empty)
	{
		struct double_link_node *l = double_link_pop(&m->blocks);
		if(l)
		{
			//if there is block per_thread_struct wake it up
			struct per_thread_struct *block_pts = (struct per_thread_struct *)((uint8_t*)l - sizeof(struct list_node));
			mutex_unlock(m->mtx);
			condition_signal(block_pts->cond);
			return;
		}
	}
	mutex_unlock(m->mtx);
}

static inline mq_sync_pop(mq_t m,struct per_thread_struct *pts,uint32_t timeout)
{
	mutex_lock(m->mtx);
	if(link_list_is_empty(m->share_list))
	{
		if(timeout)
		{	
			while(link_list_is_empty(m->share_list))
			{
				double_link_push(&m->blocks,&pts->block);
				if(0 != condition_timedwait(pts->cond,m->mtx,timeout))
				{
					double_link_remove(&pts->block);
					break;
				}
			}
		}
	}
	link_list_swap(pts->local_q,m->share_list);
	mutex_unlock(m->mtx);
}

void mq_push(mq_t m,struct list_node *msg)
{
	struct per_thread_struct *pts = (struct per_thread_struct*)pthread_getspecific(m->t_key);
	if(!pts)
	{
		pts = per_thread_create();
		LINK_LIST_PUSH_BACK(m->local_lists,pts);
		pthread_setspecific(m->t_key,(void*)pts);
	}
	if(0 == pts->is_associate)
	{
		struct thread_associate_mqs *tmq = (struct thread_associate_mqs*)pthread_getspecific(g_mq_system->t_key);
		if(!tmq)
		{
			tmq = (struct thread_associate_mqs*)calloc(1,sizeof(tmq));
			tmq->mqs = LINK_LIST_CREATE();
			mutex_lock(g_mq_system->mtx);
			LINK_LIST_PUSH_BACK(g_mq_system->thread_mqs,tmq);
			add_thread(g_mq_system,pthread_self());
			mutex_unlock(g_mq_system->mtx);
			pthread_setspecific(g_mq_system->t_key,(void*)tmq);	
		}
		struct thread_mq_element *ele = calloc(1,sizeof(*ele));
		ele->_mq = m;
		LINK_LIST_PUSH_BACK(tmq->mqs,ele);
		pts->is_associate = 1;
	}
	pts->block_sig = 1;
	LINK_LIST_PUSH_BACK(pts->local_q,msg);
	if(link_list_size(pts->local_q) >= m->push_size)
		mq_sync_push(m,pts);
	pts->block_sig = 0;			
}


struct list_node* mq_pop(mq_t m,uint32_t timeout)
{
	struct per_thread_struct *pts = (struct per_thread_struct*)pthread_getspecific(m->t_key);
	if(!pts)
	{
		pts = per_thread_create();
		LINK_LIST_PUSH_BACK(m->local_lists,pts);
		pthread_setspecific(m->t_key,(void*)pts);
	}
	if(link_list_is_empty(pts->local_q))
		mq_sync_pop(m,pts,timeout);
	return LINK_LIST_POP(struct list_node*,pts->local_q);
}


void   mq_swap(mq_t m,struct link_list *l,uint32_t timeout)
{
	struct per_thread_struct *pts = (struct per_thread_struct*)pthread_getspecific(m->t_key);
	if(!pts)
	{
		pts = per_thread_create();
		LINK_LIST_PUSH_BACK(m->local_lists,pts);
		pthread_setspecific(m->t_key,(void*)pts);
	}
	if(link_list_is_empty(pts->local_q))
		mq_sync_pop(m,pts,timeout);
	link_list_swap(l,pts->local_q);
}


void mq_force_sync(mq_t m)
{
	struct per_thread_struct *pts = (struct per_thread_struct*)pthread_getspecific(m->t_key);
	if(pts->block_sig == 0)
	{	
		if(link_list_is_empty(pts->local_q) == 0)
			mq_sync_push(m,pts);
	}
}

static void signal_handler(int sig)
{
	struct thread_associate_mqs *tmq = (struct thread_associate_mqs*)pthread_getspecific(g_mq_system->t_key);
	if(tmq)
	{
		list_node *n = link_list_head(tmq->mqs);
		while(n)
		{
			mq_t _mq = ((struct thread_mq_element*)n)->_mq;
			mq_force_sync(_mq);
			n = n->next;
		}
	}
}

