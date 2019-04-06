/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#define _GNU_SOURCE /* for accept4 */
#include <fluent-bit.h>
#include <fluent-bit/flb_info.h>
#include "flb_tests_runtime.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

struct kube_test {
    flb_ctx_t *flb;
};

struct kube_test_result {
    const char *target;
    const char *suffix;
    int   type;
    int   nMatched;
};

/* Test target mode */
#define KUBE_TAIL     0
#define KUBE_SYSTEMD  1

#ifdef FLB_HAVE_SYSTEMD
int flb_test_systemd_send(void);
char kube_test_id[64];
#endif

/* Constants */
#define KUBE_IP       "127.0.0.1"
#define KUBE_PORT     "8002"
#define KUBE_URL      "http://" KUBE_IP ":" KUBE_PORT
#define DPATH         FLB_TESTS_DATA_PATH "data/kubernetes"

static int file_to_buf(const char *path, char **out_buf, size_t *out_size)
{
    int ret;
    long bytes;
    char *buf;
    FILE *fp;
    struct stat st;

    ret = stat(path, &st);
    if (ret == -1) {
        return -1;
    }

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    buf = flb_malloc(st.st_size);
    if (!buf) {
        flb_errno();
        fclose(fp);
        return -1;
    }

    bytes = fread(buf, st.st_size, 1, fp);
    if (bytes != 1) {
        flb_errno();
        flb_free(buf);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *out_buf = buf;
    *out_size = st.st_size;

    return 0;
}

/* Given a target, lookup the .out file and return it content in a new buffer */
static char *get_out_file_content(const char *target, const char *suffix)
{
    int ret;
    char file[PATH_MAX];
    char *p;
    char *out_buf;
    size_t out_size;

    if (suffix) {
        snprintf(file, sizeof(file) - 1, DPATH "/out/%s_%s.out", target, suffix);
    }
    else {
        snprintf(file, sizeof(file) - 1, DPATH "/out/%s.out", target);
    }

    ret = file_to_buf(file, &out_buf, &out_size);
    TEST_CHECK_(ret == 0, "getting output file content: %s", file);
    if (ret != 0) {
        return NULL;
    }

    /* Sanitize content, get rid of ending \n */
    p = out_buf + (out_size - 1);
    while (*p == '\n' || *p == '\r') p--;
    *++p = '\0';

    return out_buf;
}

static int cb_check_result(void *record, size_t size, void *data)
{
    struct kube_test_result *result;
    char *out;
    char *check;
    char streamfilter[64] = {'\0'};

    result = (struct kube_test_result *) data;
    out = get_out_file_content(result->target, result->suffix);
    if (!out) {
        return -1;
    }
    if (result->type == KUBE_SYSTEMD) {
        char *skip_record, *skip_out;
        /* Skip the other records since some are created by systemd,
           only check the kubernetes annotations
         */
        skip_out = strstr(out, "\"kubernetes\":");
        skip_record = strstr(record, "\"kubernetes\":");
        if (skip_out && skip_record) {
            TEST_CHECK(strcmp(skip_record, skip_out) == 0);
            TEST_MSG("skip_record: %s\nskip_out: %s\n",
                     skip_record, skip_out);
            result->nMatched++;
        }
    } else {
        if (result->suffix && *result->suffix) {
            sprintf(streamfilter, "\"stream\":\"%s\"", result->suffix);
        }
        if (!*streamfilter ||
            strstr(record, streamfilter)) {

            /*
             * Our validation is: check that the content of out file is found
             * in the output record.
             */
            check = strstr(record, out);
            TEST_CHECK_(check != NULL,
                       "comparing expected record with actual record");
            if (check == NULL) {
                if (result->suffix) {
                    printf("Target: %s, suffix: %s\n",
                           result->target, result->suffix);
                }
                else
                {
                    printf("Target: %s\n",
                           result->target);
                }
                printf("Expected record:\n%s\n"
                       "Actual record:\n%s\n",
                       out, (char *)record);
            }
            result->nMatched++;
        }
    }
    if (size > 0) {
        flb_free(record);
    }
    flb_free(out);
    return 0;
}

static void kube_test(const char *target, int type, const char *suffix, int nExpected, ...)
{
    int ret;
    int in_ffd;
    int filter_ffd;
    int out_ffd;
    char *key;
    char *value;
    char path[PATH_MAX];
    va_list va;
    struct kube_test ctx;
    struct flb_lib_out_cb cb_data;
    struct kube_test_result result = {0};

    result.nMatched = 0;
    result.target = target;
    result.suffix = suffix;
    result.type = type;

    /* Compose path based on target */
    snprintf(path, sizeof(path) - 1, DPATH "/log/%s.log", target);

    TEST_CHECK_(access(path, R_OK) == 0, "accessing log file: %s", path);

    ctx.flb = flb_create();
    TEST_CHECK_(ctx.flb != NULL, "initialising service");
    if (!ctx.flb) {
        goto exit;
    }

    ret = flb_service_set(ctx.flb,
                          "Flush", "1",
                          "Grace", "1",
                          "Log_Level", "error",
                          "Parsers_File", DPATH "/parsers.conf",
                          NULL);
    TEST_CHECK_(ret == 0, "setting service options");

    if (type == KUBE_TAIL) {
        in_ffd = flb_input(ctx.flb, "tail", NULL);
        TEST_CHECK_(in_ffd >= 0, "initialising input");
        ret = flb_input_set(ctx.flb, in_ffd,
                            "Tag", "kube.<namespace>.<pod>.<container>",
                            "Tag_Regex", "^(?<namespace>.+)_(?<pod>.+)_(?<container>.+)\\.log$",
                            "Path", path,
                            "Parser", "docker",
                            "Docker_Mode", "On",
                            NULL);
        TEST_CHECK_(ret == 0, "setting input options");
    }
#ifdef FLB_HAVE_SYSTEMD
    else if (type == KUBE_SYSTEMD) {
        sprintf(kube_test_id, "KUBE_TEST=%u%lu", getpid(), random());
        in_ffd = flb_input(ctx.flb, "systemd", NULL);
        TEST_CHECK_(in_ffd >= 0, "initialising input");
        ret = flb_input_set(ctx.flb, in_ffd,
                            "Tag", "kube.*",
                            "Systemd_Filter", kube_test_id,
                            NULL);
        TEST_CHECK_(ret == 0, "setting input options");
    }
#endif

    filter_ffd = flb_filter(ctx.flb, "kubernetes", NULL);
    TEST_CHECK_(filter_ffd >= 0, "initialising filter");
    ret = flb_filter_set(ctx.flb, filter_ffd,
                         "Match", "kube.*",
                         "Kube_Url", KUBE_URL,
                         "Kube_Meta_Preload_Cache_Dir", DPATH "/meta",
                         NULL);
    TEST_CHECK_(ret == 0, "setting filter options");

    /* Iterate number of arguments for filter_kubernetes additional options */
    va_start(va, nExpected);
    while ((key = va_arg(va, char *))) {
        value = va_arg(va, char *);
        if (!value) {
            /* Wrong parameter */
            break;
        }
        ret = flb_filter_set(ctx.flb, filter_ffd, key, value, NULL);
        TEST_CHECK_(ret == 0, "setting filter additional options");
    }
    va_end(va);

    if (type == KUBE_TAIL) {
        ret = flb_filter_set(ctx.flb, filter_ffd,
                             "Regex_Parser", "kubernetes-tag",
                             NULL);
        TEST_CHECK_(ret == 0, "setting filter specific options");
    }
#ifdef FLB_HAVE_SYSTEMD
    else if (type == KUBE_SYSTEMD) {
        ret = flb_filter_set(ctx.flb, filter_ffd,
                             "Use_Journal", "On",
                             NULL);
        TEST_CHECK_(ret == 0, "setting filter specific options");
    }
#endif

    /* Prepare output callback context*/
    cb_data.cb = cb_check_result;
    cb_data.data = &result;

    /* Output */
    out_ffd = flb_output(ctx.flb, "lib", (void *) &cb_data);
    TEST_CHECK_(out_ffd >= 0, "initialising output");
    flb_output_set(ctx.flb, out_ffd,
                   "Match", "kube.*",
                   "format", "json",
                   NULL);
    TEST_CHECK_(ret == 0, "setting output options");

#ifdef FLB_HAVE_SYSTEMD
    /*
     *  If the source of data is Systemd, just let the output lib plugin
     * to process one record only, otherwise when the test case stop after
     * the first callback it destroy the contexts, but out_lib still have
     * pending data to flush. This option solves the problem.
     */
    if (type == KUBE_SYSTEMD) {
        flb_output_set(ctx.flb, out_ffd,
                       "Max_Records", "1",
                       NULL);
        TEST_CHECK_(ret == 0, "setting output specific options");
    }
#endif

    /* Start the engine */
    ret = flb_start(ctx.flb);
    TEST_CHECK_(ret == 0, "starting engine");
    if (ret == -1) {
        goto exit;
    }
#ifdef FLB_HAVE_SYSTEMD
    if (type == KUBE_SYSTEMD) {
        TEST_CHECK_(flb_test_systemd_send() >= 0,
                    "sending sample message to journal");
    }
#endif

    /* Poll for up to 2 seconds or until we got a match */
    for (ret = 0; ret < 2000 && result.nMatched == 0; ret++) {
        usleep(1000);
    }
    TEST_CHECK(result.nMatched == nExpected);
    TEST_MSG("result.nMatched: %i\nnExpected: %i", result.nMatched, nExpected);

    ret = flb_stop(ctx.flb);
    TEST_CHECK_(ret == 0, "stopping engine");

exit:
    if (ctx.flb) {
        flb_destroy(ctx.flb);
    }
}

#define flb_test_core(target, suffix, nExpected) \
    kube_test("core/" target, KUBE_TAIL, suffix, nExpected, NULL);

static void flb_test_core_base()
{
    flb_test_core("core_base_fluent-bit", NULL, 1);
}

static void flb_test_core_unescaping_text()
{
    flb_test_core("core_unescaping_text", NULL, 1);
}

static void flb_test_core_unescaping_json()
{
    flb_test_core("core_unescaping_json", NULL, 1);
}

#define flb_test_options_merge_log(target, suffix, nExpected) \
    kube_test("options/" target, KUBE_TAIL, suffix, nExpected, \
              "Merge_Log", "On", \
              NULL); \

static void flb_test_options_merge_log_text()
{
    flb_test_options_merge_log("options_merge-log_text", NULL, 1);
}

static void flb_test_options_merge_log_escaped_text()
{
    flb_test_options_merge_log("options_merge-log_escaped-text", NULL, 1);
}

static void flb_test_options_merge_log_json()
{
    flb_test_options_merge_log("options_merge-log_json", NULL, 1);
}

static void flb_test_options_merge_log_escaped_json()
{
    flb_test_options_merge_log("options_merge-log_escaped-json", NULL, 1);
}

static void flb_test_options_merge_log_invalid_json()
{
    flb_test_options_merge_log("options_merge-log_invalid-json", NULL, 1);
}

static void flb_test_options_merge_log_trim_enable()
{
    flb_test_options_merge_log("options_merge-log-trim_enable", NULL, 1);
}

static void flb_test_options_merge_log_trim_disable()
{
    flb_test_options_merge_log("options_merge-log-trim_enable", NULL, 1);
}

#define flb_test_options_merge_log_key(target, suffix, nExpected) \
    kube_test("options/" target, KUBE_TAIL, suffix, nExpected, \
              "Merge_Log", "On", \
              "Merge_Log_Key", "merge-log-key", \
              NULL); \

static void flb_test_options_merge_log_key_json()
{
    flb_test_options_merge_log_key("options_merge-log-key_json", NULL, 1);
}

#define flb_test_options_keep_log(target, suffix, nExpected) \
    kube_test("options/" target, KUBE_TAIL, suffix, nExpected, \
              "Merge_Log", "On", \
              "Keep_Log", "Off", \
              NULL); \

static void flb_test_options_keep_log_disable()
{
    flb_test_options_keep_log("options_keep-log_disable", NULL, 1);
}

#define flb_test_annotations_parser(target, suffix, nExpected) \
    kube_test("annotations/" target, KUBE_TAIL, suffix, nExpected, \
              "K8s-Logging.Parser", "On", \
              "Merge_Log", "On", \
              "Keep_Log", "Off", \
              NULL); \

static void flb_test_annotations_parser_regex_without_time_text()
{
    flb_test_annotations_parser("annotations_parser-regex-without-time_text", NULL, 1);
}

static void flb_test_annotations_parser_regex_with_time_text()
{
    flb_test_annotations_parser("annotations_parser-regex-with-time_text", NULL, 1);
}

static void flb_test_annotations_parser_regex_with_time_invalid_text_1()
{
    flb_test_annotations_parser("annotations_parser-regex-with-time_invalid-text-1", NULL, 1);
}

static void flb_test_annotations_parser_regex_with_time_invalid_text_2()
{
    flb_test_annotations_parser("annotations_parser-regex-with-time_invalid-text-2", NULL, 1);
}

static void flb_test_annotations_parser_json_without_time_json()
{
    flb_test_annotations_parser("annotations_parser-json-without-time_json", NULL, 1);
}

static void flb_test_annotations_parser_json_with_time_json()
{
    flb_test_annotations_parser("annotations_parser-json-with-time_json", NULL, 1);
}

static void flb_test_annotations_parser_json_with_time_invalid_json_1()
{
    flb_test_annotations_parser("annotations_parser-json-with-time_invalid-json-1", NULL, 1);
}

static void flb_test_annotations_parser_json_with_time_invalid_json_2()
{
    flb_test_annotations_parser("annotations_parser-json-with-time_invalid-json-2", NULL, 1);
}

static void flb_test_annotations_parser_stdout_text_stdout()
{
    flb_test_annotations_parser("annotations_parser-stdout_text", "stdout", 1);
}

static void flb_test_annotations_parser_stdout_text_stderr()
{
    flb_test_annotations_parser("annotations_parser-stdout_text", "stderr", 1);
}

static void flb_test_annotations_parser_stderr_text_stdout()
{
    flb_test_annotations_parser("annotations_parser-stderr_text", "stdout", 1);
}

static void flb_test_annotations_parser_stderr_text_stderr()
{
    flb_test_annotations_parser("annotations_parser-stderr_text", "stderr", 1);
}

static void flb_test_annotations_parser_multiple_1_container_1_stdout()
{
    flb_test_annotations_parser("annotations_parser-multiple-1_container-1", "stdout", 1);
}

static void flb_test_annotations_parser_multiple_1_container_1_stderr()
{
    flb_test_annotations_parser("annotations_parser-multiple-1_container-1", "stderr", 1);
}

static void flb_test_annotations_parser_multiple_1_container_2_stdout()
{
    flb_test_annotations_parser("annotations_parser-multiple-1_container-2", "stdout", 1);
}

static void flb_test_annotations_parser_multiple_1_container_2_stderr()
{
    flb_test_annotations_parser("annotations_parser-multiple-1_container-2", "stderr", 1);
}

static void flb_test_annotations_parser_multiple_1_container_3_stdout()
{
    flb_test_annotations_parser("annotations_parser-multiple-1_container-3", "stdout", 1);
}

static void flb_test_annotations_parser_multiple_1_container_3_stderr()
{
    flb_test_annotations_parser("annotations_parser-multiple-1_container-3", "stderr", 1);
}

static void flb_test_annotations_parser_multiple_1_container_4_stdout()
{
    flb_test_annotations_parser("annotations_parser-multiple-1_container-4", "stdout", 1);
}

static void flb_test_annotations_parser_multiple_1_container_4_stderr()
{
    flb_test_annotations_parser("annotations_parser-multiple-1_container-4", "stderr", 1);
}

static void flb_test_annotations_parser_multiple_1_container_5_stdout()
{
    flb_test_annotations_parser("annotations_parser-multiple-1_container-5", "stdout", 1);
}

static void flb_test_annotations_parser_multiple_1_container_5_stderr()
{
    flb_test_annotations_parser("annotations_parser-multiple-1_container-5", "stderr", 1);
}

static void flb_test_annotations_parser_multiple_2_container_1_stdout()
{
    flb_test_annotations_parser("annotations_parser-multiple-2_container-1", "stdout", 1);
}

static void flb_test_annotations_parser_multiple_2_container_1_stderr()
{
    flb_test_annotations_parser("annotations_parser-multiple-2_container-1", "stderr", 1);
}

static void flb_test_annotations_parser_multiple_2_container_2_stdout()
{
    flb_test_annotations_parser("annotations_parser-multiple-2_container-2", "stdout", 1);
}

static void flb_test_annotations_parser_multiple_2_container_2_stderr()
{
    flb_test_annotations_parser("annotations_parser-multiple-2_container-2", "stderr", 1);
}

static void flb_test_annotations_parser_multiple_2_container_3_stdout()
{
    flb_test_annotations_parser("annotations_parser-multiple-2_container-3", "stdout", 1);
}

static void flb_test_annotations_parser_multiple_2_container_3_stderr()
{
    flb_test_annotations_parser("annotations_parser-multiple-2_container-3", "stderr", 1);
}

static void flb_test_annotations_parser_multiple_2_container_4_stdout()
{
    flb_test_annotations_parser("annotations_parser-multiple-2_container-4", "stdout", 1);
}

static void flb_test_annotations_parser_multiple_2_container_4_stderr()
{
    flb_test_annotations_parser("annotations_parser-multiple-2_container-4", "stderr", 1);
}

static void flb_test_annotations_parser_multiple_2_container_5_stdout()
{
    flb_test_annotations_parser("annotations_parser-multiple-2_container-5", "stdout", 1);
}

static void flb_test_annotations_parser_multiple_2_container_5_stderr()
{
    flb_test_annotations_parser("annotations_parser-multiple-2_container-5", "stderr", 1);
}

#define flb_test_annotations_exclude(target, suffix, nExpected) \
    kube_test("annotations/" target, KUBE_TAIL, suffix, nExpected, \
              "K8s-Logging.Exclude", "On", \
              NULL); \

static void flb_test_annotations_exclude_text()
{
    flb_test_annotations_exclude("annotations_exclude_text", NULL, 0);
}

#ifdef FLB_HAVE_SYSTEMD
#define CONTAINER_NAME "CONTAINER_NAME=k8s_kairosdb_kairosdb-914055854-b63vq_default_d6c53deb-05a4-11e8-a8c4-080027435fb7_23"
#include <systemd/sd-journal.h>

int flb_test_systemd_send()
{
    return sd_journal_send(
            "@timestamp=2018-02-23T08:58:45.0Z",
            "PRIORITY=6",
            CONTAINER_NAME,
            "CONTAINER_TAG=",
            "CONTAINER_ID=56e257661383",
            "CONTAINER_ID_FULL=56e257661383836fac4cd90a23ee8a7a02ee1538c8f35657d1a90f3de1065a22",
            "MESSAGE=08:58:45.839 [qtp151442075-47] DEBUG [HttpParser.java:281] - filled 157/157",
            kube_test_id,
            NULL);
}

static void flb_test_systemd_logs()
{
    struct stat statb;
    /* We want to avoid possibly getting a log from a previous run,
       so create a unique-id for each 'send' */
    sprintf(kube_test_id, "KUBE_TEST=%u%lu", getpid(), random());

    if (stat("/run/systemd/journal/socket", &statb) == 0 &&
        statb.st_mode & S_IFSOCK) {

        int r;
        sd_journal *journal;
        r = sd_journal_open(&journal, 0);
        if (r < 0) {
            flb_error("Skip test: journal error: ", strerror(-r));
            return;
        }

        r = sd_journal_get_fd(journal);
        if (r < 0) {
            flb_error("Skip test: journal fd error: ", strerror(-r));
            sd_journal_close(journal);
            return;
        }
        sd_journal_add_match(journal, CONTAINER_NAME, 0);
        sd_journal_seek_tail(journal);

        /*
         * Send test message to Journal. If this fails (e.g. journal is
         * not running then skip the test.
         */
        if (flb_test_systemd_send() < 0) {

            flb_error("Skip test: journal send error: ", strerror(-r));
            sd_journal_close(journal);
            return;
        }

        r = sd_journal_previous(journal);
        if (r < 0) {
            flb_error("Skip test: journal previous error: ", strerror(-r));
            sd_journal_close(journal);
            return;
        }

        r = sd_journal_next(journal);
        if (r < 0) {
            flb_error("Skip test: journal next error: ", strerror(-r));
            sd_journal_close(journal);
            return;
        }

        r = sd_journal_wait(journal, 2000);
        if (r < 0) {
            flb_error("Skip test: journal wait error: ", strerror(-r));
            sd_journal_close(journal);
            return;
        }
        sd_journal_close(journal);

        kube_test(T_SYSTEMD_SIMPLE, KUBE_SYSTEMD, NULL, 1,
                  "Merge_Log", "On",
                  NULL);
    }
}
#endif

TEST_LIST = {
    {"kube_core_base", flb_test_core_base},
    {"kube_core_unescaping_text", flb_test_core_unescaping_text},
    {"kube_core_unescaping_json", flb_test_core_unescaping_json},
    {"kube_options_merge_log_text", flb_test_options_merge_log_text},
    {"kube_options_merge_log_escaped_text", flb_test_options_merge_log_escaped_text},
    {"kube_options_merge_log_json", flb_test_options_merge_log_json},
    {"kube_options_merge_log_escaped_json", flb_test_options_merge_log_escaped_json},
    {"kube_options_merge_log_invalid_json", flb_test_options_merge_log_invalid_json},
    {"kube_options_merge_log_trim_enable", flb_test_options_merge_log_trim_enable},
    {"kube_options_merge_log_trim_disable", flb_test_options_merge_log_trim_disable},
    {"kube_options_merge_log_key_json", flb_test_options_merge_log_key_json},
    {"kube_options_keep_log_disable", flb_test_options_keep_log_disable},
    {"kube_annotations_parser_regex_without_time_text", flb_test_annotations_parser_regex_without_time_text},
    {"kube_annotations_parser_regex_with_time_text", flb_test_annotations_parser_regex_with_time_text},
    {"kube_annotations_parser_regex_with_time_invalid_text_1", flb_test_annotations_parser_regex_with_time_invalid_text_1},
    {"kube_annotations_parser_regex_with_time_invalid_text_2", flb_test_annotations_parser_regex_with_time_invalid_text_2},
    {"kube_annotations_parser_json_without_time_json", flb_test_annotations_parser_json_without_time_json},
    {"kube_annotations_parser_json_with_time_json", flb_test_annotations_parser_json_with_time_json},
    {"kube_annotations_parser_json_with_time_invalid_json_1", flb_test_annotations_parser_json_with_time_invalid_json_1},
    {"kube_annotations_parser_json_with_time_invalid_json_2", flb_test_annotations_parser_json_with_time_invalid_json_2},
    {"kube_annotations_parser_stdout_text_stdout", flb_test_annotations_parser_stdout_text_stdout},
    {"kube_annotations_parser_stdout_text_stderr", flb_test_annotations_parser_stdout_text_stderr},
    {"kube_annotations_parser_stderr_text_stdout", flb_test_annotations_parser_stderr_text_stdout},
    {"kube_annotations_parser_stderr_text_stderr", flb_test_annotations_parser_stderr_text_stderr},
    {"kube_annotations_parser_multiple_1_container_1_stdout", flb_test_annotations_parser_multiple_1_container_1_stdout},
    {"kube_annotations_parser_multiple_1_container_1_stderr", flb_test_annotations_parser_multiple_1_container_1_stderr},
    {"kube_annotations_parser_multiple_1_container_2_stdout", flb_test_annotations_parser_multiple_1_container_2_stdout},
    {"kube_annotations_parser_multiple_1_container_2_stderr", flb_test_annotations_parser_multiple_1_container_2_stderr},
    {"kube_annotations_parser_multiple_1_container_3_stdout", flb_test_annotations_parser_multiple_1_container_3_stdout},
    {"kube_annotations_parser_multiple_1_container_3_stderr", flb_test_annotations_parser_multiple_1_container_3_stderr},
    {"kube_annotations_parser_multiple_1_container_4_stdout", flb_test_annotations_parser_multiple_1_container_4_stdout},
    {"kube_annotations_parser_multiple_1_container_4_stderr", flb_test_annotations_parser_multiple_1_container_4_stderr},
    {"kube_annotations_parser_multiple_1_container_5_stdout", flb_test_annotations_parser_multiple_1_container_5_stdout},
    {"kube_annotations_parser_multiple_1_container_5_stderr", flb_test_annotations_parser_multiple_1_container_5_stderr},
    {"kube_annotations_parser_multiple_2_container_1_stdout", flb_test_annotations_parser_multiple_2_container_1_stdout},
    {"kube_annotations_parser_multiple_2_container_1_stderr", flb_test_annotations_parser_multiple_2_container_1_stderr},
    {"kube_annotations_parser_multiple_2_container_2_stdout", flb_test_annotations_parser_multiple_2_container_2_stdout},
    {"kube_annotations_parser_multiple_2_container_2_stderr", flb_test_annotations_parser_multiple_2_container_2_stderr},
    {"kube_annotations_parser_multiple_2_container_3_stdout", flb_test_annotations_parser_multiple_2_container_3_stdout},
    {"kube_annotations_parser_multiple_2_container_3_stderr", flb_test_annotations_parser_multiple_2_container_3_stderr},
    {"kube_annotations_parser_multiple_2_container_4_stdout", flb_test_annotations_parser_multiple_2_container_4_stdout},
    {"kube_annotations_parser_multiple_2_container_4_stderr", flb_test_annotations_parser_multiple_2_container_4_stderr},
    {"kube_annotations_parser_multiple_2_container_5_stdout", flb_test_annotations_parser_multiple_2_container_5_stdout},
    {"kube_annotations_parser_multiple_2_container_5_stderr", flb_test_annotations_parser_multiple_2_container_5_stderr},
    {"kube_annotations_exclude_text", flb_test_annotations_exclude_text},
#ifdef FLB_HAVE_SYSTEMD
    {"kube_systemd_logs", flb_test_systemd_logs},
#endif
    {NULL, NULL}
};
