#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <curl/curl.h> // for HTTP request
#include <unistd.h>
#include <sys/stat.h> //yhe fie k info nikalny k liye use ke h (like file size,file permission,type,time)
#include <time.h>
#include <errno.h>
#include <sys/statvfs.h> //yhe file system k stats (free space etc) disk ke free and total space k liye

// Constants
#define MAX_THREADS 4           // max 4 thread aik sath parts me file download karen ge
#define BUFFER_SIZE 1024 * 1024 // 1MB buffer
#define MAX_URL_LENGTH 2048     // URL max  char
#define MAX_FILENAME_LENGTH 256 // file name length 256  max

// Structure to hold thread data
typedef struct
{
    curl_off_t start;
    curl_off_t end;
    curl_off_t initial_start;
    int thread_id;
    char *url;
    FILE *fp;
    double *progress;  // download progress 0.0 se 1.0 tak
    time_t start_time; // thread ne download karna kab shuru kiya
    int *error_flag;   // error ahya to flag set hojata
    curl_off_t *bytes_downloaded;
} ThreadData;

// Global variables
pthread_mutex_t file_mutex;
sem_t buffer_sem;
double global_progress[MAX_THREADS];
curl_off_t global_bytes_downloaded[MAX_THREADS]; // har thread ne kitny  byte download kiye uska record
int total_threads;
time_t global_start_time;
int global_error_flag = 0;

// ANSI color codes
#define GREEN "\033[32m" // progess line
#define RED "\033[31m"   // for error
#define CYAN "\033[36m"  // for file download header
#define RESET "\033[0m"  // text ka color wapas default

// Error message printing function
void print_error(const char *message)
{
    printf("%s[ERROR]%s %s\n\n", RED, RESET, message);
}

// Styled header
void print_header()
{
    printf("\n\t\t%s=====================================%s\n", CYAN, RESET);
    printf("\t\t%s      File Download Manager      %s\n", CYAN, RESET);
    printf("\t\t%s=====================================%s\n\n", CYAN, RESET);
}

// Convert bytes to human-readable format
void bytes_to_human_readable(curl_off_t bytes, char *buffer, size_t buffer_size)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = (double)bytes;
    int unit_index = 0;

    while (size >= 1024 && unit_index < 4)
    {
        size /= 1024;
        unit_index++;
    }

    snprintf(buffer, buffer_size, "%.2f %s", size, units[unit_index]);
}

// Check storage space
int check_storage_space(curl_off_t file_size)
{
    if (file_size <= 0)
    {
        printf("  File size unknown, skipping storage check...\n\n");
        return 1;
    }

    struct statvfs stat;
    if (statvfs(".", &stat) != 0) // "." represent current directory jahan file save hogi
    {
        print_error("Failed to check storage space");
        return 0;
    }

    unsigned long long free_space = (unsigned long long)stat.f_bavail * stat.f_frsize; // free block and block size
    if (free_space < (unsigned long long)file_size)
    {
        char msg[256]; // buffer for error msge
        snprintf(msg, sizeof(msg), "Insufficient storage space. Need %lld bytes, available %llu bytes",
                 (long long)file_size, free_space);
        print_error(msg);
        return 0;
    }
    return 1;
}

// Progress bar function
void print_progress_bar(double *progress, int num_threads, curl_off_t *bytes_downloaded, curl_off_t total_size)
{
    if (total_size > 0) // if filesize known
    {
        const int bar_width = 50;
        double total_progress = 0;

        for (int i = 0; i < num_threads; i++)
        {
            total_progress += progress[i];
        }
        total_progress /= num_threads; // average progress nikalny

        if (total_progress < 0.0)
            total_progress = 0.0;
        if (total_progress > 1.0)
            total_progress = 1.0; // agar 1 se zyda h to set 1 (100)% download

        int pos = bar_width * total_progress;
        time_t current_time = time(NULL);
        double elapsed = difftime(current_time, global_start_time); // download shuru hony se ab tak ka time

        double eta = 0;
        if (total_progress > 0 && total_progress < 1)
        {
            eta = (elapsed / total_progress) * (1.0 - total_progress); // estimate time for download
        }

        printf("\r  %s[", GREEN);
        for (int i = 0; i < bar_width; ++i)
        {
            if (i < pos)
                printf("=");
            else
                printf(" ");
        }
        printf("]%s %.2f%% (ETA: %.0fs)", RESET, total_progress * 100, eta);
    }
    else // if total size unknown
    {
        curl_off_t total_bytes = 0;
        for (int i = 0; i < num_threads; i++)
        {
            total_bytes += bytes_downloaded[i];
        }
        double mb_downloaded = total_bytes / (1024.0 * 1024.0);                          // Moved calculation before printf
        printf("\r  %s[Downloading]%s %.2f MB downloaded", GREEN, RESET, mb_downloaded); // Use mb_downloaded after calculation
    }
    fflush(stdout); // yhe ensure karta h output immediatelyconsole me dekhy
}

// Callback for curl write function
size_t write_data(void *ptr, size_t size, size_t nmemb, void *stream)
{
    ThreadData *data = (ThreadData *)stream;
    size_t written;                              // yeh track karta hai ke kitni data file me likh gaye

    pthread_mutex_lock(&file_mutex);
    if (fseek(data->fp, data->start, SEEK_SET) != 0)
    {
        print_error("Failed to seek in output file");
        *data->error_flag = 1;
        pthread_mutex_unlock(&file_mutex);
        return 0;
    }

    written = fwrite(ptr, size, nmemb, data->fp);
    if (written != size * nmemb)
    {
        print_error("Failed to write to output file");
        *data->error_flag = 1;
        pthread_mutex_unlock(&file_mutex);
        return 0;
    }

    data->start += written;
    data->bytes_downloaded[data->thread_id] += written;

    if (data->end > 0)
    {
        curl_off_t total = data->end - data->initial_start;
        curl_off_t downloaded = data->start - data->initial_start; // kitna download hogaya h
        double current = (double)downloaded / total;               // progress calculate karta hai (0.0 se 1.0 ke beech)

        if (current < 0.0)
            current = 0.0;
        if (current > 1.0)
            current = 1.0;

        data->progress[data->thread_id] = current;
    }

    pthread_mutex_unlock(&file_mutex);
    sem_post(&buffer_sem);

    return written; // kitna data likh gaya ,yeh curl ko bataaya hai
}

// Thread function to download a segment-->File badi hai aur multi-threading se download speed badh sakti ha
void *download_segment(void *arg)
{
    ThreadData *data = (ThreadData *)arg;
    CURL *curl;
    CURLcode res;

    curl = curl_easy_init(); // Curl handle banata hai download ke liye
    if (!curl)
    {
        print_error("Curl initialization failed");
        *data->error_flag = 1;
        return NULL;
    }

    char range[64];
    snprintf(range, sizeof(range), "%lld-%lld", (long long)data->start, (long long)data->end);

    curl_easy_setopt(curl, CURLOPT_URL, data->url);                       // Download URL set karta hai
    curl_easy_setopt(curl, CURLOPT_RANGE, range);                         // Range set karta hai (sirf specific part download hoga)
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);            // write_data function ko call karta hai data save karne ke liye
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);                      // ThreadData structure pass karta hai
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);                   // Redirects follow karta h
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);                      // HTTP errors par fail karta hai
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);                  // 10 seconds ka timeout set karta ha
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FileDownloadManager/1.0"); // Program ka user-agent set karta hai

    int retries = 3; // 3 baar retry karega agar error aata hai
    while (retries > 0)
    {
        res = curl_easy_perform(curl); // Download start karta hai
        if (res == CURLE_OK)
            break;

        char msg[256];
        if (res == CURLE_COULDNT_CONNECT || res == CURLE_OPERATION_TIMEDOUT)
        {
            snprintf(msg, sizeof(msg), "Thread %d: Internet connectivity issue, retrying... (%d left)",
                     data->thread_id, retries);
        }
        else if (res == CURLE_HTTP_RETURNED_ERROR) // HTTP error (jaise 404, 403)
        {
            long response_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            snprintf(msg, sizeof(msg), "Thread %d: HTTP error %ld, retrying... (%d left)",
                     data->thread_id, response_code, retries);
        }
        else
        {
            snprintf(msg, sizeof(msg), "Thread %d: Download failed (%s), retrying... (%d left)",
                     data->thread_id, curl_easy_strerror(res), retries);
        }
        print_error(msg);
        retries--;
        sleep(2);
    }

    if (res != CURLE_OK)
    {
        char msg[256];
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        snprintf(msg, sizeof(msg), "Thread %d: Download failed after retries: %s (HTTP %ld)",
                 data->thread_id, curl_easy_strerror(res), response_code);
        print_error(msg);
        *data->error_flag = 1;
    }

    curl_easy_cleanup(curl); // Curl handle ko free karta ha
    return NULL;
}

// Single-threaded download function -->File chhoti hai, to multi-threading ki zaroorat nah
int download_single_threaded(const char *url, FILE *fp, double *progress, curl_off_t *bytes_downloaded)
{
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        print_error("Curl initialization failed");
        return 1;
    }

    ThreadData data = {0};
    data.start = 0;
    data.initial_start = 0;
    data.end = 0;
    data.thread_id = 0;
    data.url = (char *)url;
    data.fp = fp;
    data.progress = progress;
    data.bytes_downloaded = bytes_downloaded;
    data.error_flag = &global_error_flag;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FileDownloadManager/1.0");

    global_start_time = time(NULL);
    progress[0] = 0.0;
    bytes_downloaded[0] = 0;
    total_threads = 1;

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
    {
        char msg[256];
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        snprintf(msg, sizeof(msg), "Single-threaded download failed: %s (HTTP %ld)",
                 curl_easy_strerror(res), response_code);
        print_error(msg);
        curl_easy_cleanup(curl);
        return 1;
    }

    curl_easy_cleanup(curl);
    return 0;
}

// Yeh function check karta hai ke server range requests support karta hai ya nahi
int supports_range_requests(const char *url)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-1");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FileDownloadManager/1.0");

    CURLcode res = curl_easy_perform(curl); // request bhejta ha server pe
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code); // HTTP response code leta hai (e.g., 206, 200)

    curl_easy_cleanup(curl);

    return (res == CURLE_OK && response_code == 206);
}

// Get file size and content type from URL
curl_off_t get_file_size(const char *url, char *content_type, size_t content_type_size)
{
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        print_error("Curl initialization failed");
        return -2;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // Sirf headers mangta hai (data download nahi karta)
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FileDownloadManager/1.0");

    CURLcode res = curl_easy_perform(curl); // Server se headers mangta hai
    curl_off_t file_size = -1;
    long response_code = 0;

    if (res == CURLE_OK)
    {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (response_code != 200)
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "Cannot access URL: HTTP %ld (e.g., 404 Not Found, 403 Forbidden)", response_code);
            print_error(msg);
            curl_easy_cleanup(curl);
            return -2;
        }

        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &file_size); // File ka size bytes mein leta hai
        if (file_size == -1)
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "Content-Length header missing (HTTP %ld), proceeding with single-threaded download", response_code);
            printf("%s[WARNING]%s %s\n\n", CYAN, RESET, msg);
        }

        // Get Content-Type and remove parameters like qs
        char *ct = NULL;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct); // Content type leta hai (e.g., "video/mp4")
        if (ct)
        {
            // Copy the content type up to any parameters (e.g., "; qs=0.001")
            char *param_start = strchr(ct, ';');
            if (param_start)
            {
                // Copy only the part before the parameters
                size_t len = param_start - ct;
                if (len >= content_type_size)
                    len = content_type_size - 1;
                strncpy(content_type, ct, len);
                content_type[len] = '\0';
            }
            else
            {
                // No parameters, copy the whole string
                strncpy(content_type, ct, content_type_size - 1);
                content_type[content_type_size - 1] = '\0';
            }
        }
        else
        {
            strcpy(content_type, "unknown");
        }
    }
    else // agar requestfail hui
    {
        char msg[256];
        if (res == CURLE_COULDNT_CONNECT || res == CURLE_OPERATION_TIMEDOUT)
        {
            snprintf(msg, sizeof(msg), "Failed to get file size: Internet connectivity issue");
        }
        else if (res == CURLE_HTTP_RETURNED_ERROR)
        {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            snprintf(msg, sizeof(msg), "Cannot access URL: HTTP %ld (e.g., 404 Not Found, 403 Forbidden)", response_code);
        }
        else
        {
            snprintf(msg, sizeof(msg), "Failed to get file size: %s", curl_easy_strerror(res));
        }
        print_error(msg);
        curl_easy_cleanup(curl);
        return -2;
    }

    curl_easy_cleanup(curl);
    return file_size;
}

// Yeh function URL se file ka naam (filename) aur extension (file type) extract karta haiL
void get_filename_from_url(const char *url, char *filename, size_t max_len, char *file_extension, size_t ext_len)
{
    const char *last_slash = strrchr(url, '/');
    if (last_slash && *(last_slash + 1))
    {
        strncpy(filename, last_slash + 1, max_len - 1);
    }
    else
    {
        snprintf(filename, max_len, "download_%ld", time(NULL));
    }
    filename[max_len - 1] = '\0';

    const char *last_dot = strrchr(filename, '.');
    if (last_dot && *(last_dot + 1))
    {
        strncpy(file_extension, last_dot + 1, ext_len - 1);
        file_extension[ext_len - 1] = '\0';
    }
    else
    {
        strcpy(file_extension, "unknown");
    }
}

int main(int argc, char *argv[])
{
    pthread_mutex_init(&file_mutex, NULL);
    sem_init(&buffer_sem, 0, MAX_THREADS);
    curl_global_init(CURL_GLOBAL_ALL);

    print_header();

    if (argc != 2)
    {
        print_error("Usage: ./download_manager <URL>");
        pthread_mutex_destroy(&file_mutex);
        sem_destroy(&buffer_sem);
        curl_global_cleanup();
        return 1;
    }

    const char *url = argv[1];

    if (strlen(url) == 0)
    {
        print_error("URL cannot be empty");
        pthread_mutex_destroy(&file_mutex);
        sem_destroy(&buffer_sem);
        curl_global_cleanup();
        return 1;
    }

    if (strlen(url) >= MAX_URL_LENGTH)
    {
        print_error("URL is too long");
        pthread_mutex_destroy(&file_mutex);
        sem_destroy(&buffer_sem);
        curl_global_cleanup();
        return 1;
    }

    printf("  URL: %s\n\n", url); // Indented and spaced

    global_error_flag = 0;
    global_start_time = time(NULL);

    char content_type[128] = {0};
    curl_off_t file_size = get_file_size(url, content_type, sizeof(content_type));
    if (file_size < 0 && file_size != -1)
    {
        print_error("Cannot proceed with download due to URL accessibility issues");
        pthread_mutex_destroy(&file_mutex);
        sem_destroy(&buffer_sem);
        curl_global_cleanup();
        return 1;
    }

    // Display file info with aligned labels
    printf("  File Information:\n");
    if (file_size > 0)
    {
        char size_str[32];
        bytes_to_human_readable(file_size, size_str, sizeof(size_str));
        printf("    Size        : %s\n", size_str);
    }
    else
    {
        printf("    Size        : Unknown\n");
    }
    printf("    Content Type: %s\n\n", content_type); // Added newline for spacing

    if (file_size > 0 && !check_storage_space(file_size))
    {
        pthread_mutex_destroy(&file_mutex);
        sem_destroy(&buffer_sem);
        curl_global_cleanup();
        return 1;
    }

    char filename[MAX_FILENAME_LENGTH];
    char file_extension[32];
    get_filename_from_url(url, filename, MAX_FILENAME_LENGTH, file_extension, sizeof(file_extension));
    FILE *fp = fopen(filename, "wb");
    if (!fp)
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to create output file: %s", strerror(errno));
        print_error(msg);
        pthread_mutex_destroy(&file_mutex);
        sem_destroy(&buffer_sem);
        curl_global_cleanup();
        return 1;
    }

    printf("  Saving as: %s\n", filename);         // Moved here for flow
    printf("  Extension: %s\n\n", file_extension); // Aligned and spaced

    if (file_size == -1)
    {
        printf("  Mode: Single-threaded (unknown file size)\n\n"); // Indented and spaced
        global_progress[0] = 0.0;
        global_bytes_downloaded[0] = 0;
        if (download_single_threaded(url, fp, global_progress, global_bytes_downloaded) != 0)
        {
            fclose(fp);
            remove(filename);
            pthread_mutex_destroy(&file_mutex);
            sem_destroy(&buffer_sem);
            curl_global_cleanup();
            return 1;
        }
    }
    else
    {
        int use_multithreading = supports_range_requests(url);

        if (use_multithreading)
        {
            printf("  Mode: Multithreaded (%d threads)\n\n", MAX_THREADS);

            pthread_t threads[MAX_THREADS];
            ThreadData thread_data[MAX_THREADS];
            curl_off_t segment_size = file_size / MAX_THREADS;
            total_threads = MAX_THREADS;

            for (int i = 0; i < MAX_THREADS; i++)
            {
                thread_data[i].start = i * segment_size;
                thread_data[i].initial_start = thread_data[i].start;
                thread_data[i].end = (i == MAX_THREADS - 1) ? file_size : (i + 1) * segment_size;
                thread_data[i].thread_id = i;
                thread_data[i].url = (char *)url;
                thread_data[i].fp = fp;
                thread_data[i].progress = global_progress;
                thread_data[i].bytes_downloaded = global_bytes_downloaded;
                thread_data[i].start_time = global_start_time;
                thread_data[i].error_flag = &global_error_flag;
                global_progress[i] = 0.0;
                global_bytes_downloaded[i] = 0;

                if (pthread_create(&threads[i], NULL, download_segment, &thread_data[i]) != 0)
                {
                    print_error("Failed to create thread");
                    fclose(fp);
                    pthread_mutex_destroy(&file_mutex);
                    sem_destroy(&buffer_sem);
                    curl_global_cleanup();
                    return 1;
                }
            }

            // Progress bar update loop
            while (1)
            {
                print_progress_bar(global_progress, MAX_THREADS, global_bytes_downloaded, file_size);
                if (global_error_flag)
                    break;

                int done = 1;
                for (int i = 0; i < MAX_THREADS; i++)
                {
                    if (global_progress[i] < 1.0)
                    {
                        done = 0;
                        break;
                    }
                }
                if (done)
                    break;
                usleep(100000);
            }

            // Join threads
            for (int i = 0; i < MAX_THREADS; i++)
            {
                pthread_join(threads[i], NULL);
            }
        }
        else
        {
            printf("  Mode: Single-threaded (range requests not supported)\n\n");
            global_progress[0] = 0.0;
            global_bytes_downloaded[0] = 0;
            if (download_single_threaded(url, fp, global_progress, global_bytes_downloaded) != 0)
            {
                fclose(fp);
                remove(filename);
                pthread_mutex_destroy(&file_mutex);
                sem_destroy(&buffer_sem);
                curl_global_cleanup();
                return 1;
            }
        }

        // Add spacing after the loop (no need for a final print_progress_bar call)
        printf("\n\n"); // Ensure spacing after the last progress bar

        if (global_error_flag)
        {
            print_error("Download failed");
            fclose(fp);
            remove(filename);
            pthread_mutex_destroy(&file_mutex);
            sem_destroy(&buffer_sem);
            curl_global_cleanup();
            return 1;
        }

        // Download summary with aligned labels
        time_t end_time = time(NULL);
        double elapsed = difftime(end_time, global_start_time);
        curl_off_t total_bytes_downloaded = 0;
        for (int i = 0; i < ((file_size == -1 || !supports_range_requests(url)) ? 1 : MAX_THREADS); i++)
        {
            total_bytes_downloaded += global_bytes_downloaded[i];
        }

        struct stat file_stat;
        printf("  Download Summary:\n"); // Spaced header
        if (stat(filename, &file_stat) == 0)
        {
            char size_str[32];
            bytes_to_human_readable(file_stat.st_size, size_str, sizeof(size_str)); // file_stat.st_size file ka size byte me deta ha jo MB
            printf("    File Saved    : %s\n", filename);
            printf("    Final Size    : %s\n", size_str);
            printf("    Extension     : %s\n", file_extension);
            printf("    Content Type  : %s\n", content_type);

            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&file_stat.st_ctime));
            printf("    Created       : %s\n", time_str);

            double mb_downloaded = total_bytes_downloaded / (1024.0 * 1024.0);
            double speed = (elapsed > 0) ? mb_downloaded / elapsed : 0;
            printf("    Time Taken    : %.2f seconds\n", elapsed);
            printf("    Average Speed : %.2f MB/s\n", speed);
        }
        else
        {
            print_error("Failed to retrieve file stats");
        }

        printf("\n  Download completed successfully!\n\n"); // Spaced final message

        fclose(fp);
        pthread_mutex_destroy(&file_mutex);
        sem_destroy(&buffer_sem);
        curl_global_cleanup();

        return 0;
    }
}
