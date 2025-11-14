#include "process_scheduler.h"

// --- Global Değişken Tanımlamaları ---
Process* all_processes_head = NULL;
Process* ready_queue_head = NULL;
Process* waiting_queue_head = NULL;

pthread_mutex_t all_processes_mutex; // YENİ
pthread_mutex_t ready_queue_mutex;
pthread_mutex_t waiting_queue_mutex;
pthread_mutex_t output_mutex;

long long global_clock_start_time;
int total_processes = 0;
int terminated_processes_count = 0;
int all_processes_loaded = 0;

// --- Zamanlama Fonksiyonları ---
long long get_milliseconds() {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday basarisiz");
        exit(EXIT_FAILURE);
    }
    return (long long)tv.tv_sec * 1000 + (long long)tv.tv_usec / 1000;
}

long long get_current_clock() {
    return get_milliseconds() - global_clock_start_time;
}

// --- Kuyruk Yönetim Fonksiyonları ---
void enqueue(Process** head, Process* p, pthread_mutex_t* mutex) {
    if (pthread_mutex_lock(mutex) != 0) {
        perror("enqueue: mutex lock"); exit(EXIT_FAILURE);
    }
    p->next = NULL;
    if (*head == NULL) {
        *head = p;
    } else {
        Process* temp = *head;
        while (temp->next != NULL) {
            temp = temp->next;
        }
        temp->next = p;
    }
    if (pthread_mutex_unlock(mutex) != 0) {
        perror("enqueue: mutex unlock"); exit(EXIT_FAILURE);
    }
}

// --- Olay Yöneticisi Thread (I/O ve Varışlar) ---
void* event_manager_thread(void* arg) {
    (void)arg;
    
    while (1) {
        int found_io_done = 0;
        int found_arrival = 0;
        long long current_clock = get_current_clock();
        Process* finished_process = NULL;
        Process* arrived_process = NULL;

        // --- 1. I/O'su bitenleri kontrol et ---
        if (pthread_mutex_lock(&waiting_queue_mutex) != 0) { perror("event: wait lock"); exit(1); }
        Process* temp_wait = waiting_queue_head;
        Process* prev_wait = NULL;
        while (temp_wait != NULL) {
            if (current_clock >= temp_wait->io_finish_time) {
                finished_process = temp_wait;
                if (prev_wait == NULL) waiting_queue_head = temp_wait->next;
                else prev_wait->next = temp_wait->next;
                temp_wait->next = NULL;
                found_io_done = 1;
                break; // Her döngüde bir tane
            }
            prev_wait = temp_wait;
            temp_wait = temp_wait->next;
        }
        if (pthread_mutex_unlock(&waiting_queue_mutex) != 0) { perror("event: wait unlock"); exit(1); }

        // --- 2. Yeni gelen prosesleri kontrol et ---
        if (pthread_mutex_lock(&all_processes_mutex) != 0) { perror("event: all_proc lock"); exit(1); }
        Process* temp_new = all_processes_head;
        Process* prev_new = NULL;
        while (temp_new != NULL) {
             if (temp_new->state == NEW && temp_new->arrival_time <= current_clock) {
                arrived_process = temp_new;
                if (prev_new == NULL) all_processes_head = temp_new->next;
                else prev_new->next = temp_new->next;
                temp_new->next = NULL;
                found_arrival = 1;
                break; // Her döngüde bir tane
             }
             prev_new = temp_new;
             temp_new = temp_new->next;
        }
        // Tüm prosesler yüklendi mi diye bak
        if (all_processes_head == NULL && !all_processes_loaded) {
            all_processes_loaded = 1;
        }
        if (pthread_mutex_unlock(&all_processes_mutex) != 0) { perror("event: all_proc unlock"); exit(1); }
        
        // --- 3. I/O'su biteni READY'ye taşı ---
        if (finished_process != NULL) {
            finished_process->state = READY;
            finished_process->time_entered_ready = current_clock; // Aging için zamanı ayarla

            if (pthread_mutex_lock(&output_mutex) != 0) { perror("event: output lock 1"); exit(1); }
            printf(MSG_IO_DONE, current_clock, finished_process->pid);
            printf(MSG_READY, current_clock, finished_process->pid);
            if (pthread_mutex_unlock(&output_mutex) != 0) { perror("event: output unlock 1"); exit(1); }
            
            enqueue(&ready_queue_head, finished_process, &ready_queue_mutex);
        }

        // --- 4. Yeni geleni READY'ye taşı ---
        if (arrived_process != NULL) {
            arrived_process->state = READY;
            // Önemli: Aging sayacı, prosesin *gerçek* varış/hazır zamanında başlar 
            arrived_process->time_entered_ready = arrived_process->arrival_time; 
            
            if (pthread_mutex_lock(&output_mutex) != 0) { perror("event: output lock 2"); exit(1); }
            // Önemli Düzeltme: Her iki log da prosesin gerçek arrival_time'ını kullanır 
            printf(MSG_ARRIVED, (long long)arrived_process->arrival_time, arrived_process->pid);
            printf(MSG_READY, (long long)arrived_process->arrival_time, arrived_process->pid);
            if (pthread_mutex_unlock(&output_mutex) != 0) { perror("event: output unlock 2"); exit(1); }

            enqueue(&ready_queue_head, arrived_process, &ready_queue_mutex);
        }
        
        // --- 5. Bitiş Koşulu ---
        // all_processes_loaded: tüm prosesler dosyadan geldi ve `ready`e taşındı
        // terminated_processes_count: `main` thread tüm bu prosesleri bitirdi
        if (all_processes_loaded && terminated_processes_count == total_processes) {
            break;
        }

        // --- 6. Uyku ---
        // Eğer hiçbir iş yapılmadıysa (I/O bitmedi, yeni proses gelmedi)
        // CPU'yu yormamak için 1ms uyu
        if (!found_io_done && !found_arrival) {
            usleep(1000); 
        }
        // Eğer iş yapıldıysa, uyuma, hemen tekrar kontrol et (başka olaylar olabilir)
    }
    return NULL;
}


// --- Ana Thread (Zamanlayıcı) Fonksiyonları ---
void apply_aging(long long current_clock) {
    if (pthread_mutex_lock(&ready_queue_mutex) != 0) { perror("aging: lock"); exit(1); }
    
    Process* temp = ready_queue_head;
    while (temp != NULL) {
        // [cite: 32]
        if ((current_clock - temp->time_entered_ready) >= AGING_THRESHOLD) {
            if (temp->priority > 0) { // 0 en yüksek [cite: 29]
                temp->priority--;
            }
            temp->time_entered_ready = current_clock; // Zamanlayıcıyı sıfırla [cite: 33]
        }
        temp = temp->next;
    }
    
    if (pthread_mutex_unlock(&ready_queue_mutex) != 0) { perror("aging: unlock"); exit(1); }
}

Process* find_and_remove_best_process() {
    Process* best_process = NULL;
    Process* best_prev = NULL;
    Process* temp;
    Process* prev = NULL;
    
    if (pthread_mutex_lock(&ready_queue_mutex) != 0) { perror("find: lock"); exit(1); }
    
    if (ready_queue_head == NULL) {
        if (pthread_mutex_unlock(&ready_queue_mutex) != 0) { perror("find: unlock empty"); exit(1); }
        return NULL; // Kuyruk boş
    }

    // En iyi prosesi bul (Priority-SRTF)
    best_process = ready_queue_head;
    temp = ready_queue_head->next;
    prev = ready_queue_head;
    
    while (temp != NULL) {
        // Kriter 1: Öncelik [cite: 29]
        if (temp->priority < best_process->priority) {
            best_process = temp;
            best_prev = prev;
        } 
        // Kriter 2: Eşit Öncelik -> SRTF [cite: 30]
        else if (temp->priority == best_process->priority) {
            if (temp->remaining_cpu_time < best_process->remaining_cpu_time) {
                best_process = temp;
                best_prev = prev;
            }
        }
        prev = temp;
        temp = temp->next;
    }

    // En iyi prosesi listeden çıkar
    if (best_prev == NULL) { // Listenin başı seçilmiş
        ready_queue_head = best_process->next;
    } else {
        best_prev->next = best_process->next;
    }
    best_process->next = NULL;
    
    if (pthread_mutex_unlock(&ready_queue_mutex) != 0) { perror("find: unlock found"); exit(1); }
    
    return best_process;
}

// --- Ana Fonksiyon (Scheduler ve CPU) ---
int main(int argc, char *argv[]) {
    FILE *dosya;
    char satir[MAX_LINE_LENGTH];

    if (argc != 2) {
        fprintf(stderr, "Kullanim: %s <input_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *dosya_adi = argv[1];
    dosya = fopen(dosya_adi, "r");
    if (dosya == NULL) {
        perror("Hata: Dosya acilamadi"); 
        return EXIT_FAILURE;
    }

    // 1. Prosesleri dosyadan oku ve 'all_processes' listesine ekle
    while (fgets(satir, MAX_LINE_LENGTH, dosya) != NULL) {
        if (satir[0] == '\n' || satir[0] == '#') continue;

        Process* p = (Process*)malloc(sizeof(Process));
        if (p == NULL) { perror("Hata: malloc"); fclose(dosya); return EXIT_FAILURE; }
        
        int items = sscanf(satir, "%d %d %d %d %d %d",
               &p->pid, &p->arrival_time, &p->total_cpu_time,
               &p->interval_time, &p->io_time, &p->priority);

        if (items != 6) { // [cite: 13]
            fprintf(stderr, "Hata: Gecersiz satir formati: %s", satir); free(p); continue; 
        }
        
        p->remaining_cpu_time = p->total_cpu_time;
        p->state = NEW;
        p->next = NULL;

        // Başlangıçta listeye ekle (thread yok, mutex gerekmez)
        if (all_processes_head == NULL) all_processes_head = p;
        else {
            Process* temp = all_processes_head;
            while(temp->next != NULL) temp = temp->next;
            temp->next = p;
        }
        total_processes++;
    }
    fclose(dosya);

    if (total_processes == 0) {
        fprintf(stderr, "Hata: Dosyada hic proses bulunamadi.\n");
        return EXIT_FAILURE;
    }

    // 2. Mutex'leri ve Olay Yöneticisi Thread'ini başlat
    if (pthread_mutex_init(&all_processes_mutex, NULL) != 0 || // YENİ
        pthread_mutex_init(&ready_queue_mutex, NULL) != 0 ||
        pthread_mutex_init(&waiting_queue_mutex, NULL) != 0 ||
        pthread_mutex_init(&output_mutex, NULL) != 0) {
        perror("Hata: Mutex init"); return EXIT_FAILURE;
    }

    pthread_t event_thread; // Adı değişti
    if (pthread_create(&event_thread, NULL, event_manager_thread, NULL) != 0) { // Fonksiyon adı değişti
        perror("Hata: Event thread olusturulamadi"); return EXIT_FAILURE;
    }

    // 3. Global saati başlat
    global_clock_start_time = get_milliseconds();

    // 4. Ana Zamanlayıcı Döngüsü (CPU) [cite: 54, 55]
    while (terminated_processes_count < total_processes) {
        long long current_clock = get_current_clock();

        // Adım A: Yaşlandırmayı uygula (Artık varışları kontrol etmiyoruz)
        apply_aging(current_clock);

        // Adım B: Çalıştırılacak prosesi seç
        Process* running_process = find_and_remove_best_process();

        if (running_process != NULL) {
            // --- Proses bulundu ve çalıştırılıyor ---
            running_process->state = RUNNING;
            
            int burst_time = running_process->interval_time;
            if (running_process->remaining_cpu_time < burst_time) {
                burst_time = running_process->remaining_cpu_time;
            }
            
            if (pthread_mutex_lock(&output_mutex) != 0) { perror("main: output lock"); exit(1); }
            printf(MSG_DISPATCHED, current_clock, running_process->pid, 
                   running_process->priority, running_process->remaining_cpu_time, burst_time);
            if (pthread_mutex_unlock(&output_mutex) != 0) { perror("main: output unlock"); exit(1); }

            // CPU burst'ünü simüle et (Bu sırada event_thread çalışıyor)
            usleep(burst_time * 1000); 

            running_process->remaining_cpu_time -= burst_time;
            long long after_burst_clock = get_current_clock(); // Uykudan sonraki yeni zaman

            // Adım C: Prosesin durumunu kontrol et
            if (running_process->remaining_cpu_time <= 0) {
                // --- PROSES TAMAMLANDI --- [cite: 24]
                running_process->state = TERMINATED;
                terminated_processes_count++;
                
                if (pthread_mutex_lock(&output_mutex) != 0) { perror("main: term lock"); exit(1); }
                printf(MSG_TERMINATED, after_burst_clock, running_process->pid);
                if (pthread_mutex_unlock(&output_mutex) != 0) { perror("main: term unlock"); exit(1); }
                
                free(running_process); 
            
            } else {
                // --- PROSES I/O İÇİN BLOKLANDI --- [cite: 21]
                running_process->state = WAITING;
                running_process->io_finish_time = after_burst_clock + running_process->io_time;
                
                if (pthread_mutex_lock(&output_mutex) != 0) { perror("main: block lock"); exit(1); }
                printf(MSG_BLOCKED, after_burst_clock, running_process->pid, running_process->io_time);
                if (pthread_mutex_unlock(&output_mutex) != 0) { perror("main: block unlock"); exit(1); }

                enqueue(&waiting_queue_head, running_process, &waiting_queue_mutex); // [cite: 22]
            }
        } else {
            // --- Çalıştırılacak proses yok (Ready Queue boş) ---
            // CPU boşta. Event thread'inin çalışması için 1ms uyu.
            usleep(1000);
        }
        
        // all_processes_loaded bayrağı artık event_thread tarafından yönetiliyor
    }

    // 5. Temizlik
    if (pthread_join(event_thread, NULL) != 0) {
        perror("Hata: Event thread join");
        return EXIT_FAILURE;
    }

    pthread_mutex_destroy(&all_processes_mutex); // YENİ
    pthread_mutex_destroy(&ready_queue_mutex);
    pthread_mutex_destroy(&waiting_queue_mutex);
    pthread_mutex_destroy(&output_mutex);

    return 0;
}
