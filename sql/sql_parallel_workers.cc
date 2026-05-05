/*
   Copyright (c) 2025, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA */

/**
  @file

    Contains
*/


#include "sql_parallel_workers.h"

void fill_with_rubbish(pwt_queued_event **event, THD *thd)
{
  *event= (pwt_queued_event*) my_malloc(PSI_INSTRUMENT_ME,
                                           sizeof(pwt_queued_event),
                                           MYF(0));
  (*event)->next= nullptr;
  (*event)->type= pwt_queued_event::queued_event_t::QUEUED_WARNING;
  (*event)->warning= (pwt_warning_message*) my_malloc(PSI_INSTRUMENT_ME,
                                               sizeof(pwt_warning_message),
                                               MYF(0));
  (*event)->warning->code= 100+100.0*rand()/RAND_MAX;
  (*event)->warning->level= Sql_condition::enum_warning_level::WARN_LEVEL_WARN;
  sprintf((*event)->warning->message, "%s, error, %u",
          thd->connection_name.str,
          (*event)->warning->code);
}


static void *parallel_worker_thread_func(void *arg)
{
  THD *thd= ((struct pwt_args*)arg)->thd;
  pwt_messages *messages= ((struct pwt_args*)arg)->messages;

  void *save= thd_attach_thd(thd);
  my_thread_set_name(thd->connection_name.str);
  my_sleep(1000000+1000000*(double)rand()/RAND_MAX); /* 1-11 seconds */

  if (messages->event_queue)
  {
    pwt_queued_event *last= messages->event_queue;

    while( last->next )
      last= last->next;

    fill_with_rubbish(&last->next, thd);
    messages->last_in_queue= last->next;
  }
  else
  {
    fill_with_rubbish(&messages->event_queue, thd);
    messages->last_in_queue= messages->event_queue;
  }

  my_sleep(1000000+1000000*(double)rand()/RAND_MAX); /* 1-11 seconds */
  ((struct pwt_args*)arg)->finished= true;
  thd_detach_thd(save);
  destroy_background_thd(thd);
  return nullptr;
}


bool pwt_management::init_parallel_workers(THD *thd)
{
  if (const uint nworkers= opt_parallel_worker_threads)
  {
    workers= (struct pwt_worker *) my_malloc(PSI_INSTRUMENT_ME,
                                     nworkers * sizeof(struct pwt_worker),
                                     MYF(0));
    if (!workers)
      return false;

    for (uint i= 0; i < nworkers; i++)
    {
      workers[i].arg.thd= create_background_thd();
      workers[i].arg.messages= &parallel_messages;
      workers[i].arg.thd->system_thread= SYSTEM_THREAD_SLAVE_BACKGROUND;
      size_t len= my_snprintf(workers[i].conn_name, MAX_THREAD_NAME,
                              "worker %u", i);
      workers[i].arg.thd->connection_name.str= workers[i].conn_name;
      workers[i].arg.thd->connection_name.length= len;
      workers[i].arg.finished= workers[i].joined= false;

      if (mysql_thread_create(0, &workers[i].pthread, nullptr,
                              parallel_worker_thread_func, &workers[i].arg))
      {
        destroy_background_thd(workers[i].arg.thd);
        for (uint j= 0; j < i; j++)
        {
          destroy_background_thd(workers[j].arg.thd);
          pthread_join(workers[j].pthread, nullptr);
        }
        workers= nullptr;
        return false;
      }
    }
  }
  return true;
}


void pwt_management::join_parallel_workers(THD *thd)
{
  // braindead style of waiting for all the threads to finish
  bool all_done= true;          // breakpoint outside loop
  do
  {
    all_done= true;
    for (uint i= 0; i < opt_parallel_worker_threads; i++)
    {
      if (workers[i].arg.finished)
      {
        if (!workers[i].joined)
        {
          pthread_join(workers[i].pthread, nullptr);
          workers[i].joined= true;
        }
      }
      else
        all_done= false;
    }
    my_sleep(100);
  } while (!all_done);

  // messages

  pwt_queued_event *event= parallel_messages.event_queue, *last;

  while( event )
  {
    switch(event->type)
    {
      case pwt_queued_event::QUEUED_ERROR:
        my_free(event->error);
        break;
      case pwt_queued_event::QUEUED_WARNING:
        push_warning(thd, event->warning->level, event->warning->code, event->warning->message);
        my_free(event->warning);
        break;
      case pwt_queued_event::QUEDED_DATA:
        break;
    }
    last= event;
    event= event->next;
    my_free(last);
  }

  if (opt_parallel_worker_threads)
    my_free(workers);
}

