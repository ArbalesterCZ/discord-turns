#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <regex.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <curl/curl.h>

#define LINE_SIZE 1024
#define EVENT_BUF_SIZE 4096
#define MAX_PLAYERS 8
#define NAME_SIZE 128

static char player_names[MAX_PLAYERS][NAME_SIZE];

static void trim_newline(char *s)
{
    size_t len = strlen(s);

    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        --len;
    }
}

static int load_player_names(const char *players_file)
{
    FILE *file = fopen(players_file, "r");
    if (file == NULL) {
        perror("fopen players_file");
        return -1;
    }

    char line[NAME_SIZE];
    int count = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        trim_newline(line);

        if (line[0] == '\0')
            continue;

        if (count >= MAX_PLAYERS) {
            fprintf(stderr, "The file containing the names has too many players. The maximum is %d.\n", MAX_PLAYERS);
            fclose(file);
            return -1;
        }

        snprintf(player_names[count], sizeof(player_names[count]), "%s", line);
        ++count;
    }

    fclose(file);

    if (count == 0) {
        fprintf(stderr, "The file containing the names does not include any players.\n");
        return -1;
    }

    return 0;
}

static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;

    for (size_t i = 0; src[i] != '\0' && j + 2 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == '"' || c == '\\') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c == '\n') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            // ignore CR
        } else {
            dst[j++] = c;
        }
    }

    dst[j] = '\0';
}

static int send_webhook(const char *webhook_url, const char *message, const char *username)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init failed\n");
        return -1;
    }

    char escaped_message[2048];
    char escaped_username[256];
    char payload[2600];

    json_escape(message, escaped_message, sizeof(escaped_message));
    json_escape(username, escaped_username, sizeof(escaped_username));

    snprintf(payload, sizeof(payload), "{\"content\":\"%s\",\"username\":\"%s\"}", escaped_message, escaped_username);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, webhook_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);

    CURLcode res = curl_easy_perform(curl);

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "Error sending the webhook: %s\n", curl_easy_strerror(res));
        return -1;
    }

    if (response_code < 200 || response_code >= 300) {
        fprintf(stderr, "Discord returned an HTTP status: %ld\n", response_code);
        return -1;
    }

    return 0;
}

static void handle_truncate(FILE *file, const char *filename)
{
    struct stat st;

    if (stat(filename, &st) != 0) {
        perror("stat");
        clearerr(file);
        return;
    }

    long pos = ftell(file);
    if (pos < 0) {
        clearerr(file);
        return;
    }

    if (pos > st.st_size) {
        fprintf(stderr, "The log has been truncated or cleared; resetting the read position to the beginning.\n");

        if (fseek(file, 0, SEEK_SET) != 0)
            perror("fseek after shortening the file");

        clearerr(file);
    }
}

static void game_format(char *usr, const size_t usr_len, const int days)
{
    const int month = days / 28;
    const int week = days % 28 / 7;
    const int day = days % 7;

    snprintf(usr, usr_len, "Month %d┃Week %d┃Day %d", month+1, week+1, day+1);
}

static void read_new_lines(FILE *file, regex_t *turn_regex, regex_t *days_regex, const char *webhook_url)
{
    char line[LINE_SIZE];

    while (fgets(line, sizeof(line), file) != NULL) {
        regmatch_t matches[2];

        if (regexec(turn_regex, line, 2, matches, 0) == 0)
            send_webhook(webhook_url, ":hourglass:", player_names[line[matches[1].rm_so] - '0']);
        else if (regexec(days_regex, line, 2, matches, 0) == 0) {
            char usr[64];

            snprintf(usr, sizeof(usr), "%.*s", (int)(matches[1].rm_eo - matches[1].rm_so), line + matches[1].rm_so);
            game_format(usr, sizeof(usr), atoi(usr)-1);
            send_webhook(webhook_url, ":sunny::new_moon::waxing_crescent_moon::first_quarter_moon::waxing_gibbous_moon::full_moon::waning_gibbous_moon::last_quarter_moon::waning_crescent_moon::new_moon::sunny:", usr);
        }
    }

    clearerr(file);
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <log_file> <players_file> <discord_webhook_url>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    const char *players_file = argv[2];
    const char *webhook_url = argv[3];

    if (load_player_names(players_file) != 0)
        return 1;

    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("fopen log_file");
        return 1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(file);
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "curl_global_init failed\n");
        fclose(file);
        return 1;
    }

    int inotify_fd = inotify_init1(0);
    if (inotify_fd < 0) {
        perror("inotify_init1");
        curl_global_cleanup();
        fclose(file);
        return 1;
    }

    int watch_fd = inotify_add_watch(inotify_fd, filename, IN_MODIFY | IN_ATTRIB);

    if (watch_fd < 0) {
        perror("inotify_add_watch");
        close(inotify_fd);
        curl_global_cleanup();
        fclose(file);
        return 1;
    }

    regex_t turn_regex;
    regex_t days_regex;

    regcomp(&turn_regex, "Player ([0-7]) ended his turn", REG_EXTENDED);
    regcomp(&days_regex, "Turn ([0-9]+)",                 REG_EXTENDED);

    char event_buf[EVENT_BUF_SIZE];

    while (1) {
        ssize_t len = read(inotify_fd, event_buf, sizeof(event_buf));

        if (len < 0) {
            if (errno == EINTR)
                continue;

            perror("read");
            break;
        }

        for (char *ptr = event_buf; ptr < event_buf + len; ) {
            struct inotify_event *event = (struct inotify_event *)ptr;

            if (event->mask & (IN_MODIFY | IN_ATTRIB)) {
                handle_truncate(file, filename);
                read_new_lines(file, &turn_regex, &days_regex, webhook_url);
            }

            ptr += sizeof(struct inotify_event) + event->len;
        }
    }

    inotify_rm_watch(inotify_fd, watch_fd);
    close(inotify_fd);

    curl_global_cleanup();
    regfree(&turn_regex);
    regfree(&days_regex);
    fclose(file);

    return 0;
}
