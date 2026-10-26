#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_onboard_logic.h"

static void expect_normalized(const char *input, const char *expected)
{
    char out[SOLAR_OS_ONBOARD_ENDPOINT_MAX];
    const bool ok = solar_os_onboard_normalize_endpoint(input, out, sizeof(out));
    if (!ok || strcmp(out, expected) != 0) {
        fprintf(stderr, "normalize(\"%s\") = %s \"%s\", expected \"%s\"\n",
                input, ok ? "ok" : "fail", out, expected);
    }
    assert(ok);
    assert(strcmp(out, expected) == 0);
}

static void expect_rejected(const char *input)
{
    char out[SOLAR_OS_ONBOARD_ENDPOINT_MAX];
    assert(!solar_os_onboard_normalize_endpoint(input, out, sizeof(out)));
}

static void test_presets(void)
{
    assert(solar_os_onboard_preset_count() >= 2U);
    const solar_os_onboard_preset_t *first = solar_os_onboard_preset_at(0);
    assert(first != NULL && first->endpoint != NULL);
    assert(solar_os_onboard_preset_index_for_endpoint(first->endpoint) == 0);
    assert(solar_os_onboard_preset_index_for_endpoint("http://10.0.0.2/v1/chat/completions") == -1);
    assert(solar_os_onboard_preset_index_for_endpoint("") == -1);
    assert(solar_os_onboard_preset_index_for_endpoint(NULL) == -1);
    assert(solar_os_onboard_preset_at(solar_os_onboard_preset_count()) == NULL);

    bool has_custom = false;
    for (size_t i = 0; i < solar_os_onboard_preset_count(); i++) {
        has_custom |= solar_os_onboard_preset_at(i)->endpoint == NULL;
    }
    assert(has_custom);
}

static void test_normalize(void)
{
    expect_normalized("192.168.1.10:11434", "http://192.168.1.10:11434/v1/chat/completions");
    expect_normalized("  https://api.openai.com/v1/responses  ", "https://api.openai.com/v1/responses");
    expect_normalized("http://host/v1", "http://host/v1/chat/completions");
    expect_normalized("http://host/v1/", "http://host/v1/chat/completions");
    expect_normalized("http://host/", "http://host/v1/chat/completions");
    expect_normalized("http://host", "http://host/v1/chat/completions");
    expect_normalized("HTTPS://Host:8443/api/chat/completions", "https://Host:8443/api/chat/completions");
    expect_normalized("lmstudio.local:1234/v1", "http://lmstudio.local:1234/v1/chat/completions");

    expect_rejected("");
    expect_rejected("   ");
    expect_rejected("ftp://example.com/v1");
    expect_rejected("http://");
    expect_rejected("http:///v1");
    expect_rejected("http://host/a b");
    expect_rejected("http://host/\x01");

    char long_input[400];
    memset(long_input, 'a', sizeof(long_input) - 1U);
    long_input[sizeof(long_input) - 1U] = '\0';
    expect_rejected(long_input);

    char small[16];
    assert(!solar_os_onboard_normalize_endpoint("http://host/v1", small, sizeof(small)));
}

static void test_models_url(void)
{
    char out[SOLAR_OS_ONBOARD_ENDPOINT_MAX];
    assert(solar_os_onboard_models_url("https://api.openai.com/v1/responses", out, sizeof(out)));
    assert(strcmp(out, "https://api.openai.com/v1/models") == 0);
    assert(solar_os_onboard_models_url("http://10.0.0.2:11434/v1/chat/completions", out, sizeof(out)));
    assert(strcmp(out, "http://10.0.0.2:11434/v1/models") == 0);
    assert(solar_os_onboard_models_url("http://h/v1/completions", out, sizeof(out)));
    assert(strcmp(out, "http://h/v1/models") == 0);
    assert(solar_os_onboard_models_url("http://h/responses", out, sizeof(out)));
    assert(strcmp(out, "http://h/models") == 0);

    assert(!solar_os_onboard_models_url("http://h/v1/chat/completions?x=1", out, sizeof(out)));
    assert(!solar_os_onboard_models_url("http://h/v1/generate", out, sizeof(out)));
    assert(!solar_os_onboard_models_url("http://responses", out, sizeof(out)));
    assert(!solar_os_onboard_models_url("/v1/responses", out, sizeof(out)));

    char tiny[8];
    assert(!solar_os_onboard_models_url("http://h/v1/responses", tiny, sizeof(tiny)));
    assert(tiny[0] == '\0');
}

static void test_endpoint_host(void)
{
    char host[SOLAR_OS_ONBOARD_HOST_MAX];
    uint16_t port = 0;
    bool tls = false;
    assert(solar_os_onboard_endpoint_host("https://api.openai.com/v1/responses", host, sizeof(host), &port, &tls));
    assert(strcmp(host, "api.openai.com") == 0 && port == 443 && tls);
    assert(solar_os_onboard_endpoint_host("http://10.0.0.2:11434/v1", host, sizeof(host), &port, &tls));
    assert(strcmp(host, "10.0.0.2") == 0 && port == 11434 && !tls);
    assert(solar_os_onboard_endpoint_host("http://[fe80::1]:8080/x", host, sizeof(host), &port, &tls));
    assert(strcmp(host, "fe80::1") == 0 && port == 8080);
    assert(solar_os_onboard_endpoint_host("http://h", host, sizeof(host), &port, NULL));
    assert(strcmp(host, "h") == 0 && port == 80);

    assert(!solar_os_onboard_endpoint_host("http://h:99999/", host, sizeof(host), &port, &tls));
    assert(!solar_os_onboard_endpoint_host("http://h:0/", host, sizeof(host), &port, &tls));
    assert(!solar_os_onboard_endpoint_host("http://h:/", host, sizeof(host), &port, &tls));
    assert(!solar_os_onboard_endpoint_host("http:///v1", host, sizeof(host), &port, &tls));
    assert(!solar_os_onboard_endpoint_host("ws://h/", host, sizeof(host), &port, &tls));
    assert(!solar_os_onboard_endpoint_host("http://h:80x/", host, sizeof(host), &port, &tls));
    char short_host[4];
    assert(!solar_os_onboard_endpoint_host("http://longname/", short_host, sizeof(short_host), &port, &tls));
}

static void test_parse_models(void)
{
    const char *json =
        "{\"object\":\"list\",\"data\":["
        "{\"id\":\"gpt-a\",\"object\":\"model\",\"meta\":{\"tags\":[1,2,{\"x\":\"}\"}]}},"
        "{\"object\":\"model\",\"id\":\"text-embedding-3-large\"},"
        "{\"id\":\"tts-1\"},"
        "{\"id\":\"whisper-1\"},"
        "{\"id\":\"gpt-c\",\"created\":1700000000,\"ok\":true,\"none\":null},"
        "{\"id\":\"esc\\\"aped\\u0041\"},"
        "42,"
        "{\"id\":\"gpt-b\"}"
        "],\"has_more\":false}";
    solar_os_onboard_model_list_t list;
    assert(solar_os_onboard_parse_models(json, strlen(json), NULL, &list));
    assert(list.count == 4);
    assert(list.seen == 4);
    assert(strcmp(list.ids[0], "gpt-c") == 0);
    assert(strcmp(list.ids[1], "gpt-b") == 0);
    assert(strcmp(list.ids[2], "gpt-a") == 0);
    assert(strcmp(list.ids[3], "esc\"apedA") == 0);
    assert(solar_os_onboard_model_list_contains(&list, "gpt-b"));
    assert(!solar_os_onboard_model_list_contains(&list, "tts-1"));

    assert(solar_os_onboard_parse_models(json, strlen(json), "gpt-a", &list));
    assert(list.count == 4);
    assert(strcmp(list.ids[0], "gpt-a") == 0);
    assert(strcmp(list.ids[1], "gpt-c") == 0);

    assert(solar_os_onboard_parse_models(json, strlen(json), "not-there", &list));
    assert(strcmp(list.ids[0], "gpt-c") == 0);

    /* More chat models than fit: the greatest are kept, preferred is pinned. */
    char big[4096];
    size_t n = (size_t)snprintf(big, sizeof(big), "{\"data\":[");
    for (int i = 0; i < 40; i++) {
        n += (size_t)snprintf(big + n, sizeof(big) - n, "%s{\"id\":\"m%02d\"}", i ? "," : "", i);
    }
    n += (size_t)snprintf(big + n, sizeof(big) - n, "]}");
    assert(solar_os_onboard_parse_models(big, n, "m03", &list));
    assert(list.seen == 40);
    assert(list.count == SOLAR_OS_ONBOARD_MODEL_LIST_MAX);
    assert(strcmp(list.ids[0], "m03") == 0);
    assert(strcmp(list.ids[1], "m39") == 0);
    assert(strcmp(list.ids[SOLAR_OS_ONBOARD_MODEL_LIST_MAX - 1U], "m25") == 0);

    const char *empty = "{\"data\":[]}";
    assert(solar_os_onboard_parse_models(empty, strlen(empty), NULL, &list));
    assert(list.count == 0 && list.seen == 0);

    const char *duplicate = "{\"data\":[{\"id\":\"x\"},{\"id\":\"x\"}]}";
    assert(solar_os_onboard_parse_models(duplicate, strlen(duplicate), NULL, &list));
    assert(list.count == 1);

    const char *bad[] = {
        "",
        "[]",
        "{}",
        "{\"models\":[]}",
        "{\"data\":[{\"id\":\"x\"}",
        "{\"data\":[{\"id\":\"x\",}]}",
        "{\"data\":[{\"id\":\"bad\\q\"}]}",
        "{\"data\":{}}",
        "{\"data\":[{\"id\":\"x\"}] garbage",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(!solar_os_onboard_parse_models(bad[i], strlen(bad[i]), NULL, &list));
        assert(list.count == 0);
    }

    /* Length bounds the scan even without a terminator in range. */
    const char *truncated = "{\"data\":[{\"id\":\"abc\"}]}";
    assert(!solar_os_onboard_parse_models(truncated, 12, NULL, &list));
    assert(!solar_os_onboard_parse_models_partial(truncated, 12, NULL, &list));

    /* A cut-off download keeps models whose objects were complete. */
    const char *cutoff = "{\"data\":[{\"id\":\"gpt-a\"},{\"id\":\"gpt-b";
    assert(!solar_os_onboard_parse_models(cutoff, strlen(cutoff), NULL, &list));
    assert(solar_os_onboard_parse_models_partial(cutoff, strlen(cutoff), NULL, &list));
    assert(list.count == 1);
    assert(strcmp(list.ids[0], "gpt-a") == 0);
    assert(solar_os_onboard_parse_models_partial(json, strlen(json), "gpt-a", &list));
    assert(strcmp(list.ids[0], "gpt-a") == 0);
    const char *junk = "{\"data\":[{\"id\":\"x\"]} garbage";
    assert(!solar_os_onboard_parse_models_partial(junk, strlen(junk), NULL, &list));

    /* Deep nesting is rejected instead of recursing without bound. */
    char deep[200];
    size_t d = (size_t)snprintf(deep, sizeof(deep), "{\"data\":[{\"x\":");
    for (int i = 0; i < 40; i++) {
        deep[d++] = '[';
    }
    deep[d] = '\0';
    assert(!solar_os_onboard_parse_models(deep, d, NULL, &list));
}

static void test_classify(void)
{
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_DNS_FAILED, 0) == SOLAR_OS_ONBOARD_CHECK_DNS);
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_CONNECT_FAILED, 0) == SOLAR_OS_ONBOARD_CHECK_CONNECT);
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_TIMEOUT, 0) == SOLAR_OS_ONBOARD_CHECK_TIMEOUT);
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_OK, 200) == SOLAR_OS_ONBOARD_CHECK_OK);
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_OK, 401) == SOLAR_OS_ONBOARD_CHECK_KEY_REJECTED);
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_OK, 403) == SOLAR_OS_ONBOARD_CHECK_KEY_REJECTED);
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_OK, 404) == SOLAR_OS_ONBOARD_CHECK_NO_MODEL_LIST);
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_OK, 429) == SOLAR_OS_ONBOARD_CHECK_RATE_LIMITED);
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_OK, 503) == SOLAR_OS_ONBOARD_CHECK_SERVER_ERROR);
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_OK, 418) == SOLAR_OS_ONBOARD_CHECK_HTTP_ERROR);
    assert(solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_OK, 0) == SOLAR_OS_ONBOARD_CHECK_HTTP_ERROR);

    for (int c = SOLAR_OS_ONBOARD_CHECK_OK; c <= SOLAR_OS_ONBOARD_CHECK_HTTP_ERROR; c++) {
        assert(solar_os_onboard_check_label((solar_os_onboard_check_t)c)[0] != '\0');
        assert(solar_os_onboard_check_advice((solar_os_onboard_check_t)c)[0] != '\0');
    }
    assert(solar_os_onboard_check_is_usable(SOLAR_OS_ONBOARD_CHECK_OK));
    assert(solar_os_onboard_check_is_usable(SOLAR_OS_ONBOARD_CHECK_NO_MODEL_LIST));
    assert(!solar_os_onboard_check_is_usable(SOLAR_OS_ONBOARD_CHECK_KEY_REJECTED));
    assert(!solar_os_onboard_check_is_usable(SOLAR_OS_ONBOARD_CHECK_DNS));
}

static void test_choice_and_mask(void)
{
    size_t index = 99;
    assert(solar_os_onboard_parse_choice("1", 3, &index) && index == 0);
    assert(solar_os_onboard_parse_choice(" 3 ", 3, &index) && index == 2);
    assert(!solar_os_onboard_parse_choice("0", 3, &index));
    assert(!solar_os_onboard_parse_choice("4", 3, &index));
    assert(!solar_os_onboard_parse_choice("1a", 3, &index));
    assert(!solar_os_onboard_parse_choice("", 3, &index));
    assert(!solar_os_onboard_parse_choice("99999999999999999999", 3, &index));
    assert(!solar_os_onboard_parse_choice("1", 0, &index));

    assert(strcmp(solar_os_onboard_wifi_reason_hint(15), "wrong password?") == 0);
    assert(strcmp(solar_os_onboard_wifi_reason_hint(202), "wrong password?") == 0);
    assert(solar_os_onboard_wifi_reason_hint(201) != NULL);
    assert(solar_os_onboard_wifi_reason_hint(0) == NULL);
    assert(solar_os_onboard_wifi_reason_hint(8) == NULL);

    char masked[16];
    solar_os_onboard_mask_secret("sk-1234567890abcd", masked, sizeof(masked));
    assert(strcmp(masked, "****abcd") == 0);
    solar_os_onboard_mask_secret("short", masked, sizeof(masked));
    assert(strcmp(masked, "****") == 0);
    solar_os_onboard_mask_secret(NULL, masked, sizeof(masked));
    assert(strcmp(masked, "****") == 0);
}

int main(void)
{
    test_presets();
    test_normalize();
    test_models_url();
    test_endpoint_host();
    test_parse_models();
    test_classify();
    test_choice_and_mask();
    puts("onboard_logic_test: ok");
    return 0;
}
