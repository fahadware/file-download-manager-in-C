## File Download Manager in C

A Introduction
This project is a multithreaded file download manager written in C. It supports segmented downloading using multiple threads for improved speed and efficiency. It automatically falls back to single-threaded download if multi-threading is not supported by the server or if file size is unknown.

-The project makes use of:

-pthreads for multithreading

-semaphores and mutexes for synchronization

-libcurl for HTTP/HTTPS file downloads

-statvfs and stat system calls to check disk space and file info

-ANSI colors for better terminal UI

## 📌 Project Overview

This project aims to simplify the file downloading process through multithreading and efficient resource usage. The application:

- Splits large files into 4 segments and downloads them in parallel using threads
- Automatically switches to single-threaded mode if range requests are not supported or file size is unknown
- Provides a live progress bar with download percentage and estimated time remaining (ETA)
- Handles network failures using a retry mechanism
- Checks available disk space before downloading to avoid storage errors

---

## 🎯 Key Features

- ✅ Multithreaded segmented downloading (4 threads)
- ✅ Automatic fallback to single-threaded mode
- ✅ Real-time progress bar with ETA
- ✅ Retry logic for unstable internet
- ✅ Disk space availability check before download
- ✅ Auto file name and extension detection from URL
- ✅ Styled CLI output using ANSI colors
- ✅ Summary report after download

---

## 🧠 Concepts Used

- **Multithreading**: using `pthread`
- **Semaphores & Mutexes**: thread safety and synchronization
- **Network communication**: using `libcurl`
- **File system APIs**: checking disk space using `statvfs`
- **Formatted terminal output**

---

## How It Works

1.Takes a URL from command line.

2. Checks file size and server support for range requests.

3. If supported:

   -Splits file into 4 segments.

   -Each segment is downloaded by a separate thread.

4.If not supported:

-Downloads file using a single thread.

5.Shows progress bar, speed, and ETA.

6.Displays summary on completion.

## 🛠 Compilation Instructions

Make sure you have libcurl installed. Then compile with:

```bash
gcc download_manager.c -o download_manager -lcurl -lpthread
```

---

## 📥 Usage

```bash
./download_manager <URL>
```

### Example:

```bash
./download_manager https://example.com/sample.zip
```

---

## 📊 Sample Output (Terminal)

```
  URL: https://example.com/sample.zip

  File Information:
    Size        : 200.34 MB
    Content Type: application/zip

  Mode: Multithreaded (4 threads)

  [=======================>         ] 72.56% (ETA: 8s)

  Download Summary:
    File Saved    : sample.zip
    Final Size    : 200.34 MB
    Extension     : zip
    Content Type  : application/zip
    Created       : 2025-07-02 19:30:51
    Time Taken    : 20.31 seconds
    Average Speed : 9.87 MB/s

  Download completed successfully!
```

---

## Error Handling

-Internet issues: Retries up to 3 times

-HTTP errors (404, 403, etc.): Graceful fallback

-Storage full: Stops before download starts

-File creation failure: Shows error with reason

## 🧪 Testing Tips

- Try with different file types (images, videos, zip files)
- Disconnect internet mid-download to test retry logic
- Use a URL that doesn't support range to check fallback to single-thread mode

---

## 👨‍💻 Author

**Fahad ALi**  
FAST National University, Karachi  

---

## 📜 License

This project is open-source under the [MIT License](LICENSE)
