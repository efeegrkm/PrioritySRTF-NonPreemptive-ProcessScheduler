#define _DEFAULT_SOURCE
#ifndef PROCESS_SCHEDULER_H
#define PROCESS_SCHEDULER_H

// Ödevde izin verilen kütüphaneler [cite: 47, 48]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <limits.h>

// --- Sabitler ve Makrolar ---
#define MAX_LINE_LENGTH 300
#define AGING_THRESHOLD 100 // ms [cite: 32]

// Çıktı Formatları [cite: 38-44]
#define MSG_ARRIVED "[Clock: %lld] PID %d arrived\n"
#define MSG_READY   "[Clock: %lld] PID %d moved to READY queue\n"
#define MSG_DISPATCHED "[Clock: %lld] Scheduler dispatched PID %d (Pr: %d, Rm: %d) for %d ms burst\n"
#define MSG_BLOCKED "[Clock: %lld] PID %d blocked for I/O for %d ms\n"
#define MSG_IO_DONE "[Clock: %lld] PID %d finished I/O\n"
#define MSG_TERMINATED "[Clock: %lld] PID %d TERMINATED\n"

// --- Veri Yapıları ---
typedef enum {
    NEW,
    READY,
    RUNNING,
    WAITING,
    TERMINATED
} ProcessState;

typedef struct Process {
    int pid;
    int arrival_time;
    int total_cpu_time;     // cpu_execution_time [cite: 15]
    int remaining_cpu_time;
    int interval_time;      // [cite: 16]
    int io_time;            // [cite: 17]
    int priority;           // [cite: 29]
    
    ProcessState state;
    long long time_entered_ready; // Aging için [cite: 31]
    long long io_finish_time;
    
    struct Process* next;
} Process;

// --- Global Değişken Bildirimleri ---
extern Process* all_processes_head; // Dosyadan okunan, henüz gelmemiş prosesler
extern Process* ready_queue_head;
extern Process* waiting_queue_head;

extern pthread_mutex_t all_processes_mutex; // YENİ: all_processes_head listesini korur
extern pthread_mutex_t ready_queue_mutex;
extern pthread_mutex_t waiting_queue_mutex;
extern pthread_mutex_t output_mutex;

extern long long global_clock_start_time;
extern int total_processes;
extern int terminated_processes_count;
extern int all_processes_loaded; // Artık io_thread tarafından ayarlanacak

// --- Fonksiyon Protototipleri ---

long long get_milliseconds();
long long get_current_clock();
void enqueue(Process** head, Process* p, pthread_mutex_t* mutex);
Process* dequeue_by_pid(Process** head, int pid);

/**
 * @brief Olay Yöneticisi Thread'i.
 * 1. Yeni gelen prosesleri (all_processes_head) READY kuyruğuna taşır.
 * 2. I/O'su biten prosesleri (waiting_queue_head) READY kuyruğuna taşır. [cite: 26]
 */
void* event_manager_thread(void* arg); // io_thread_function olarak da kalabilir, ismi netleştirdim

/**
 * @brief Hazır kuyruğundaki proseslere yaşlandırma uygular. [cite: 31]
 */
void apply_aging(long long current_clock);

/**
 * @brief Priority-SRTF [cite: 29, 30] mantığına göre en iyi prosesi bulur ve kuyruktan çıkarır.
 */
Process* find_and_remove_best_process();

#endif // PROCESS_SCHEDULER_H
