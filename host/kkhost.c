// Runs Kookong's AC encoder (libkksdk.so from the Duoqin Remote APK) outside the app. The library only talks to Java through JNIEnv, so this provides a fake one with just the calls it makes.
//
// usage: kkhost <libkksdk.so> <rid> <tags-file> < states
//   tags-file: one "tag|value" per line, tags 1001-1500 of the remote (what NormalCodeHelper passes to initRemote)
//   states: one "power mode temp wind ud func" per line
//   output: one line per state, each returned frame as hex, frames separated by spaces
// env: KKHOST_CERT (APK signing cert, default cert.der), KKHOST_TRACE=1 logs every JNI call
#include <dlfcn.h>
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_PACKAGE "com.duoqin.remote"
#define APP_KEY "AC82C0230BD3ACAFB33DDFEB36FFFF3A"

// every Java object the library sees is one of these; method and field ids are "name signature" strings
enum { O_STR = 1, O_BYTES, O_ARRAY, O_OBJ };

typedef struct {
    int type;
    int len;
    char *str;
    jbyte *bytes;
    void **items;
} obj;

static int trace;
static obj *cert;
static JNIEnv *the_env;

#define TRACE(...) do { if (trace) fprintf(stderr, "jni: " __VA_ARGS__); } while (0)

static obj *mk(int type, const char *str) {
    obj *o = calloc(1, sizeof(obj));
    o->type = type;
    if (str) {
        o->str = strdup(str);
        o->len = strlen(str);
    }
    return o;
}

static obj *mkbytes(int n) {
    obj *o = mk(O_BYTES, NULL);
    o->len = n;
    o->bytes = calloc(n ? n : 1, 1);
    return o;
}

static obj *mkarray(int n) {
    obj *o = mk(O_ARRAY, NULL);
    o->len = n;
    o->items = calloc(n ? n : 1, sizeof(void *));
    return o;
}

static int is(const void *id, const char *name) {
    size_t n = strlen(name);
    return !strncmp(id, name, n) && ((const char *) id)[n] == ' ';
}

// MessageDigest.getInstance(alg).digest(bytes); the library only uses MD5
static jobject md5(obj *in) {
    static unsigned char *(*fn)(const unsigned char *, size_t, unsigned char *);
    if (!fn) fn = dlsym(dlopen("libcrypto.so", RTLD_NOW), "MD5");
    obj *o = mkbytes(16);
    fn((unsigned char *) in->bytes, in->len, (unsigned char *) o->bytes);
    return (jobject) o;
}

// init() checks the app signature (context.getPackageManager().getPackageInfo(pkg).signatures[0].toByteArray()), then phones home to sdkapi.kookong.com through java.net.URL. Those objects are inert here, so no request is made.
static jobject call(const char *how, jobject self, jmethodID m, va_list a) {
    TRACE("%s %s\n", how, (char *) m);
    if (is(m, "getPackageName")) return (jobject) mk(O_STR, APP_PACKAGE);
    if (is(m, "toByteArray")) return (jobject) cert;
    if (is(m, "getInstance")) {
        obj *alg = va_arg(a, obj *);
        if (strcmp(alg->str, "MD5")) {
            fprintf(stderr, "kkhost: unexpected digest %s\n", alg->str);
            exit(3);
        }
        return (jobject) mk(O_OBJ, "MessageDigest");
    }
    if (is(m, "digest")) return md5(va_arg(a, obj *));
    return (jobject) mk(O_OBJ, (const char *) m);
}

static jobject CallObjectMethod(JNIEnv *e, jobject o, jmethodID m, ...) {
    va_list a;
    va_start(a, m);
    jobject r = call("CallObjectMethod", o, m, a);
    va_end(a);
    return r;
}

static jobject CallObjectMethodV(JNIEnv *e, jobject o, jmethodID m, va_list a) { return call("CallObjectMethodV", o, m, a); }

static jobject CallStaticObjectMethod(JNIEnv *e, jclass c, jmethodID m, ...) {
    va_list a;
    va_start(a, m);
    jobject r = call("CallStaticObjectMethod", NULL, m, a);
    va_end(a);
    return r;
}

// InputStream.read() on the fake connection: end of stream right away, like being offline
static jint CallIntMethodV(JNIEnv *e, jobject o, jmethodID m, va_list a) {
    TRACE("CallIntMethodV %s\n", (char *) m);
    return is(m, "read") ? -1 : 0;
}

static jobject NewObjectV(JNIEnv *e, jclass c, jmethodID m, va_list a) {
    TRACE("NewObjectV %s %s\n", ((obj *) c)->str, (char *) m);
    return (jobject) mk(O_OBJ, ((obj *) c)->str);
}

static jobject GetObjectField(JNIEnv *e, jobject o, jfieldID f) {
    TRACE("GetObjectField %s\n", (char *) f);
    if (is(f, "signatures")) {
        obj *sigs = mkarray(1);
        sigs->items[0] = mk(O_OBJ, "Signature");
        return (jobject) sigs;
    }
    return (jobject) mk(O_OBJ, (char *) f);
}

// SDKConfig.st is 1 everywhere except Xiaomi phones outside China
static jint GetStaticIntField(JNIEnv *e, jclass c, jfieldID f) {
    TRACE("GetStaticIntField %s\n", (char *) f);
    return is(f, "st") ? 1 : 0;
}

static void *member(const char *kind, const char *name, const char *sig) {
    TRACE("%s %s %s\n", kind, name, sig);
    char *s = malloc(strlen(name) + strlen(sig) + 2);
    sprintf(s, "%s %s", name, sig);
    return s;
}

static jclass FindClass(JNIEnv *e, const char *n) { TRACE("FindClass %s\n", n); return (jclass) mk(O_OBJ, n); }
static jclass GetObjectClass(JNIEnv *e, jobject o) { TRACE("GetObjectClass\n"); return (jclass) mk(O_OBJ, NULL); }
static jmethodID GetMethodID(JNIEnv *e, jclass c, const char *n, const char *s) { return member("GetMethodID", n, s); }
static jmethodID GetStaticMethodID(JNIEnv *e, jclass c, const char *n, const char *s) { return member("GetStaticMethodID", n, s); }
static jfieldID GetFieldID(JNIEnv *e, jclass c, const char *n, const char *s) { return member("GetFieldID", n, s); }
static jfieldID GetStaticFieldID(JNIEnv *e, jclass c, const char *n, const char *s) { return member("GetStaticFieldID", n, s); }

static jstring NewStringUTF(JNIEnv *e, const char *s) { TRACE("NewStringUTF\n"); return (jstring) mk(O_STR, s ? s : ""); }
static jsize GetStringUTFLength(JNIEnv *e, jstring s) { return ((obj *) s)->len; }
static const char *GetStringUTFChars(JNIEnv *e, jstring s, jboolean *copy) {
    if (copy) *copy = 0;
    return ((obj *) s)->str;
}
static void ReleaseStringUTFChars(JNIEnv *e, jstring s, const char *c) {}

static jsize GetArrayLength(JNIEnv *e, jarray a) { return ((obj *) a)->len; }
static jobjectArray NewObjectArray(JNIEnv *e, jsize n, jclass c, jobject init) {
    obj *o = mkarray(n);
    for (int i = 0; i < n; i++) o->items[i] = init;
    return (jobjectArray) o;
}
static jobject GetObjectArrayElement(JNIEnv *e, jobjectArray a, jsize i) { return ((obj *) a)->items[i]; }
static void SetObjectArrayElement(JNIEnv *e, jobjectArray a, jsize i, jobject v) { ((obj *) a)->items[i] = v; }

static jbyteArray NewByteArray(JNIEnv *e, jsize n) { return (jbyteArray) mkbytes(n); }
static jbyte *GetByteArrayElements(JNIEnv *e, jbyteArray a, jboolean *copy) {
    if (copy) *copy = 0;
    return ((obj *) a)->bytes;
}
static void ReleaseByteArrayElements(JNIEnv *e, jbyteArray a, jbyte *p, jint mode) {}
static void SetByteArrayRegion(JNIEnv *e, jbyteArray a, jsize at, jsize n, const jbyte *buf) { memcpy(((obj *) a)->bytes + at, buf, n); }

static void DeleteLocalRef(JNIEnv *e, jobject o) {}
static jboolean ExceptionCheck(JNIEnv *e) { return 0; }
static void ExceptionClear(JNIEnv *e) {}

static jint vm_GetEnv(JavaVM *vm, void **env, jint v) { *env = the_env; return JNI_OK; }
static jint vm_Attach(JavaVM *vm, void **env, void *args) { *env = the_env; return JNI_OK; }
static jint vm_Detach(JavaVM *vm) { return JNI_OK; }
static struct JNIInvokeInterface_ vm_fn = {
    .GetEnv = vm_GetEnv, .AttachCurrentThread = vm_Attach, .AttachCurrentThreadAsDaemon = vm_Attach, .DetachCurrentThread = vm_Detach,
};
static const struct JNIInvokeInterface_ *vm_ptr = &vm_fn;
static jint GetJavaVM(JNIEnv *e, JavaVM **vm) { *vm = (JavaVM *) &vm_ptr; return JNI_OK; }

// any JNI function not implemented above prints its table slot and exits, so a new need shows up loudly
#define STUB(n) static void stub##n(void) { fprintf(stderr, "kkhost: unimplemented JNI slot %d\n", n); exit(3); }
#define STUB10(a) STUB(a##0) STUB(a##1) STUB(a##2) STUB(a##3) STUB(a##4) STUB(a##5) STUB(a##6) STUB(a##7) STUB(a##8) STUB(a##9)
STUB10() STUB10(1) STUB10(2) STUB10(3) STUB10(4) STUB10(5) STUB10(6) STUB10(7) STUB10(8) STUB10(9) STUB10(10) STUB10(11)
STUB10(12) STUB10(13) STUB10(14) STUB10(15) STUB10(16) STUB10(17) STUB10(18) STUB10(19) STUB10(20) STUB10(21) STUB10(22) STUB10(23)
#define REF(n) (void *) stub##n
#define REF10(a) REF(a##0), REF(a##1), REF(a##2), REF(a##3), REF(a##4), REF(a##5), REF(a##6), REF(a##7), REF(a##8), REF(a##9)
static void *stubs[240] = {
    REF10(), REF10(1), REF10(2), REF10(3), REF10(4), REF10(5), REF10(6), REF10(7), REF10(8), REF10(9), REF10(10), REF10(11),
    REF10(12), REF10(13), REF10(14), REF10(15), REF10(16), REF10(17), REF10(18), REF10(19), REF10(20), REF10(21), REF10(22), REF10(23),
};

static struct JNINativeInterface_ fn;

static void setup_env(void) {
    void **slots = (void **) &fn;
    for (size_t i = 0; i < sizeof(fn) / sizeof(void *) && i < 240; i++) slots[i] = stubs[i];
    fn.FindClass = FindClass;
    fn.GetObjectClass = GetObjectClass;
    fn.GetMethodID = GetMethodID;
    fn.GetStaticMethodID = GetStaticMethodID;
    fn.GetFieldID = GetFieldID;
    fn.GetStaticFieldID = GetStaticFieldID;
    fn.CallObjectMethod = CallObjectMethod;
    fn.CallObjectMethodV = CallObjectMethodV;
    fn.CallStaticObjectMethod = CallStaticObjectMethod;
    fn.CallIntMethodV = CallIntMethodV;
    fn.NewObjectV = NewObjectV;
    fn.GetObjectField = GetObjectField;
    fn.GetStaticIntField = GetStaticIntField;
    fn.NewStringUTF = NewStringUTF;
    fn.GetStringUTFLength = GetStringUTFLength;
    fn.GetStringUTFChars = GetStringUTFChars;
    fn.ReleaseStringUTFChars = ReleaseStringUTFChars;
    fn.GetArrayLength = GetArrayLength;
    fn.NewObjectArray = NewObjectArray;
    fn.GetObjectArrayElement = GetObjectArrayElement;
    fn.SetObjectArrayElement = SetObjectArrayElement;
    fn.NewByteArray = NewByteArray;
    fn.GetByteArrayElements = GetByteArrayElements;
    fn.ReleaseByteArrayElements = ReleaseByteArrayElements;
    fn.SetByteArrayRegion = SetByteArrayRegion;
    fn.DeleteLocalRef = DeleteLocalRef;
    fn.ExceptionCheck = ExceptionCheck;
    fn.ExceptionClear = ExceptionClear;
    fn.GetJavaVM = GetJavaVM;
}

static obj *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    obj *o = mkbytes(65536);
    o->len = fread(o->bytes, 1, 65536, f);
    fclose(f);
    return o;
}

static obj *read_tags(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(1);
    }
    obj *tags = mkarray(256);
    tags->len = 0;
    char line[8192];
    while (fgets(line, sizeof line, f) && tags->len < 256) {
        line[strcspn(line, "\r\n")] = 0;
        if (*line) tags->items[tags->len++] = mk(O_STR, line);
    }
    fclose(f);
    return tags;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <libkksdk.so> <rid> <tags-file> < states\n", argv[0]);
        return 2;
    }
    trace = getenv("KKHOST_TRACE") != NULL;
    cert = read_file(getenv("KKHOST_CERT") ? getenv("KKHOST_CERT") : "cert.der");
    setup_env();
    const struct JNINativeInterface_ *fn_ptr = &fn;
    JNIEnv *env = (JNIEnv *) &fn_ptr;
    the_env = env;

    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "kkhost: %s\n", dlerror());
        return 1;
    }
    jboolean (*init)(JNIEnv *, jclass, jobject, jstring) = dlsym(lib, "Java_com_hzy_tvmao_ir_encode_CodeHelper_init");
    jint (*init_remote)(JNIEnv *, jclass, jint, jint, jobjectArray) = dlsym(lib, "Java_com_hzy_tvmao_ir_encode_CodeHelper_initRemote");
    jobjectArray (*enc)(JNIEnv *, jclass, jint, jint, jint, jint, jint, jint, jint, jint, jbyteArray, jstring) =
        dlsym(lib, "Java_com_hzy_tvmao_ir_encode_CodeHelper_enc");
    jclass cls = FindClass(env, "com/hzy/tvmao/ir/encode/CodeHelper");

    if (!init(env, cls, (jobject) mk(O_OBJ, "Context"), (jstring) mk(O_STR, APP_KEY))) {
        fprintf(stderr, "kkhost: library init failed (signature check)\n");
        return 1;
    }
    int rid = atoi(argv[2]);
    init_remote(env, cls, rid, 1, (jobjectArray) read_tags(argv[3]));

    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        int power, mode, temp, wind, ud, func;
        if (sscanf(line, "%d %d %d %d %d %d", &power, &mode, &temp, &wind, &ud, &func) != 6) {
            putchar('\n');
            continue;
        }
        // same arguments NormalCodeHelper.getWaveCodes passes; -1 is an unused slot, no key bytes, no expand keys
        obj *frames = (obj *) enc(env, cls, rid, power, mode, temp, wind, -1, ud, func, NULL, NULL);
        for (int i = 0; frames && i < frames->len; i++) {
            obj *b = frames->items[i];
            if (i) putchar(' ');
            for (int j = 0; b && j < b->len; j++) printf("%02x", (unsigned char) b->bytes[j]);
        }
        putchar('\n');
    }
    return 0;
}
