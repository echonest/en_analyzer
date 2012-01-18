/**
	@file
	en_analyzer~
	ben lacker - ben@echonest.com	
*/

#include "ext.h"							// standard Max include, always required
#include "ext_obex.h"						// required for new style Max object
#include "ext_path.h"
#include "z_dsp.h"
#include "buffer.h"
#include <curl/curl.h>
#include <curl/types.h>
#include <curl/easy.h>
#include <openssl/md5.h>
#include <json/json.h>
#include <sndfile.h>

static t_class *en_analyzer_class;

typedef struct _en_analyzer
{
	t_pxobject ob;
    t_buffer *b_buf;
    t_symbol *b_name;
    t_symbol *api_key;
    char *track_id;
    float b_msr;
    long b_nchans;
    long b_bframes;
    void *b_outlet1;
    void *b_outlet2;
    void *b_outlet3;
    void *b_outlet4;
    void *b_outlet5;
    void *b_outlet6;
	struct json_object *bars;
	struct json_object *beats;
	struct json_object *sections;
	struct json_object *segments;
	struct json_object *tatums;
    long tempo;
    long time_signature;
    long key;
    long mode;
    long loudness;
    long end_of_fade_in;
    long start_of_fade_out;
    long n_bars;
    long n_beats;
    long n_sections;
    long n_segments;
    long n_tatums;
} t_en_analyzer;

struct MemoryStruct {
    char *memory;
    size_t size;
};

void en_analyzer_dsp(t_en_analyzer *x, t_signal **s, short *count);
void en_analyzer_setup(t_en_analyzer *x, t_symbol *s, double sr);
void en_analyzer_refresh(t_en_analyzer *x);
void en_analyzer_length(t_en_analyzer *x, t_buffer *b);
void en_analyzer_assist(t_en_analyzer *x, void *b, long m, long a, char *s);
void en_analyzer_dblclick(t_en_analyzer *x);
void en_analyzer_free(t_en_analyzer *x);
void en_analyzer_analyze(t_en_analyzer *x);
char *write_soundfile(t_en_analyzer *x);
int check_for_existing_analysis(t_en_analyzer *x, char *filename);
int md5_file(FILE * file, unsigned char *digest);
size_t bytes2hex(const unsigned char *bytes, size_t numBytes, char *hex, size_t hexSize);
inline char nibble2hex(unsigned char b);
int upload(t_en_analyzer *x, char *filename);
static size_t WriteMemoryCallback(void *ptr, size_t size, size_t nmemb, void *data);
char *get_track_id(struct json_object *response_obj);
int get_analysis(t_en_analyzer *x, char *param_name, char *param);
char *get_analysis_url(t_en_analyzer *x, char *param_name, char *param);
int check_status(char *response);
char *extract_analysis_url_from_response(struct json_object *response_obj);
char *replace(const char *src, const char *from, const char *to);
char *get_and_parse_analysis(t_en_analyzer *x, char *analysis_url);
char *open_url(t_en_analyzer *x, char *analysis_url);
void en_analyzer_segment(t_en_analyzer *x, t_symbol *s, long arc, t_atom *argv);
void en_analyzer_bar(t_en_analyzer *x, t_symbol *s, long argc, t_atom *argv);
void en_analyzer_beat(t_en_analyzer *x, t_symbol *s, long argc, t_atom *argv);
void en_analyzer_section(t_en_analyzer *x, t_symbol *s, long argc, t_atom *argv);
void en_analyzer_tatum(t_en_analyzer *x, t_symbol *s, long argc, t_atom *argv);
void en_analyzer_quantum(t_en_analyzer *x, struct json_object *quantum_obj, long n);
void en_analyzer_global(t_en_analyzer *x);
void *en_analyzer_new(t_symbol *s, long argc, t_atom *argv);

t_symbol *ps_nothing, *ps_buffer;

#define AMPLITUDE (1.0 * 0x7F000000)

int main(void)
{	
	t_class *c;
	
	c = class_new("en_analyzer~", (method)en_analyzer_new, (method)dsp_free, 
	                (long)sizeof(t_en_analyzer), 0L, A_GIMME, 0);
    
    class_addmethod(c, (method)en_analyzer_analyze,    "analyze", 0);
    class_addmethod(c, (method)en_analyzer_dsp,        "dsp", A_CANT, 0);
    class_addmethod(c, (method)en_analyzer_length,     "length", A_CANT, 0);
    class_addmethod(c, (method)en_analyzer_setup,      "setup", A_CANT, 0);
    class_addmethod(c, (method)en_analyzer_refresh,    "refresh", A_CANT, 0);
    class_addmethod(c, (method)en_analyzer_assist,     "assist", A_CANT, 0);  
    class_addmethod(c, (method)en_analyzer_dblclick,   "dblclick", A_CANT, 0);
    class_addmethod(c, (method)en_analyzer_segment,    "segment", A_GIMME, 0);
    class_addmethod(c, (method)en_analyzer_bar,        "bar", A_GIMME, 0);
    class_addmethod(c, (method)en_analyzer_beat,       "beat", A_GIMME, 0);
    class_addmethod(c, (method)en_analyzer_section,    "section", A_GIMME, 0);
    class_addmethod(c, (method)en_analyzer_tatum,      "tatum", A_GIMME, 0);
    class_addmethod(c, (method)en_analyzer_global,     "global", 0);
    
    CLASS_ATTR_SYM(c, "api_key", 0, t_en_analyzer, api_key);
    
	class_dspinit(c);
    class_register(CLASS_BOX, c);
	en_analyzer_class = c;
    
    ps_nothing = gensym("");
    ps_buffer = gensym("buffer~");
	
	return 0;
}

void en_analyzer_dsp(t_en_analyzer *x, t_signal **sp, short *count)
{
    en_analyzer_setup(x, x->b_name, sp[0]->s_sr);
	en_analyzer_length(x, x->b_buf);
}

void en_analyzer_setup(t_en_analyzer *x, t_symbol *s, double sr)
{
	t_buffer *b;
	
    if (sr)
		x->b_msr = sr * .001;
	if (s != ps_nothing) {
		if ((b = (t_buffer *)(s->s_thing)) && ob_sym(b) == ps_buffer) {
			en_analyzer_length(x, b);
			x->b_buf = b;
		} else
			x->b_buf = 0;
	} else
		x->b_buf = 0;
	x->b_name = s;
}

void en_analyzer_refresh(t_en_analyzer *x)
{
	t_buffer *b;
	
	if (x->b_name != ps_nothing) {
		if ((b = (t_buffer *)(x->b_name->s_thing)) && ob_sym(b) == ps_buffer) {
			en_analyzer_length(x, b);
			x->b_buf = b;
		} else
			x->b_buf = 0;
	} else
		x->b_buf = 0;
    en_analyzer_length(x, x->b_buf);
}

void en_analyzer_length(t_en_analyzer *x, t_buffer *b)
{
    if (b) {
        x->b_nchans = b->b_nchans; // this is also "framesize"
		x->b_bframes = b->b_frames;
    }
}

void en_analyzer_assist(t_en_analyzer *x, void *b, long m, long a, char *s)
{
	if (m == ASSIST_INLET) { // inlet
		sprintf(s, "I am ben's inlet %ld", a);
	} 
	else {	// outlet
		switch (a) {
			case 0:	sprintf(s, "Segment Start Time in sec, Segment Duration in sec");	break;
			case 1:	sprintf(s, "Loudness Start in db, Loudness Max Time in sec, Loudness Max in db");	break;
			case 2:	sprintf(s, "Pitches");	break;
			case 3:	sprintf(s, "Timbre");	break;
		}
	}
}

void en_analyzer_dblclick(t_en_analyzer *x)
{
	t_buffer *b;
	if ((b = (t_buffer *)(x->b_name->s_thing)) && ob_sym(b) == ps_buffer)
		mess0((struct object *)b, gensym("dblclick"));
}

void en_analyzer_free(t_en_analyzer *x)
{
	;
}

void en_analyzer_analyze(t_en_analyzer *x)
{
    char *filename;
    int analysis_exists, response_code, success;
    
    en_analyzer_refresh(x);
    filename = write_soundfile(x);
    analysis_exists = check_for_existing_analysis(x, filename);
    if (analysis_exists) {
        post("Found existing analysis.");
    }
    else {
        post("Analysis not found. Uploading audio for analysis...");
        response_code = upload(x, filename);
        if (response_code == 0){
            success = get_analysis(x, "id", x->track_id);
            if (success == 1) {
                post("Analysis retrieved.");
            }
            else {
                post("Couldn't get analysis");
            }
        }
        else {
            post("Upload failed.");
        }
    }
    post("deleting tempfile");
    remove(filename);
    en_analyzer_global(x);
}

char *write_soundfile(t_en_analyzer *x)
{
    SNDFILE	*file;
    SF_INFO	sfinfo;
    int k;
    int *buffer;
    long SAMPLE_COUNT = x->b_bframes;
    char *filename;

    if (! (buffer = malloc (2 * SAMPLE_COUNT * sizeof (int)))){
        post("Malloc failed.");
    	return"";
        }

    memset(&sfinfo, 0, sizeof(sfinfo));

    /* write audio in buffer~ to a tempfile.
    make it mono, 22.5kHz */
    //sfinfo.samplerate	= x->b_msr * 1000;
    sfinfo.samplerate   = 22500;
    sfinfo.frames		= SAMPLE_COUNT / 2;
    sfinfo.channels		= 1;
    sfinfo.format		= (SF_FORMAT_WAV | SF_FORMAT_PCM_24) ;

    filename = tempnam("/tmp", "remix");
    strcat(filename, ".wav");
    if (! (file = sf_open(filename, SFM_WRITE, &sfinfo))){
        post("Error : Not able to open output file.\n");
    	return"";
    	}

    for (k = 0 ; k < SAMPLE_COUNT / 2 ; k++)
    		buffer[k] = AMPLITUDE * x->b_buf->b_samples[k * 2];

    if (sf_write_int(file, buffer, sfinfo.channels * SAMPLE_COUNT / 2) !=
    										sfinfo.channels * SAMPLE_COUNT / 2)
    	puts(sf_strerror(file));

    sf_close(file);
    post("wrote audio to tempfile: %s", filename);
    return filename;
}

int check_for_existing_analysis(t_en_analyzer *x, char *filename)
{
    FILE *tempfile;
    unsigned char digest[MD5_DIGEST_LENGTH];
    char md5_result[MD5_DIGEST_LENGTH * 2 + 1];
    int found_analysis;
    
    tempfile = fopen(filename, "r");
    md5_file(tempfile, digest);
    fclose(tempfile);
    bytes2hex(digest, sizeof(digest), md5_result, sizeof(md5_result));
    post("md5 of audio is: %s", md5_result);
    
    found_analysis = get_analysis(x, "md5", md5_result);
    return found_analysis;
}

int md5_file(FILE * file, unsigned char *digest)
{
    unsigned char buf[4096];
    MD5_CTX c;
    size_t len;

    MD5_Init(&c);
    while(len = fread(buf, sizeof(buf[0]), sizeof(buf), file))
        MD5_Update(&c, buf, len);
    MD5_Final(digest, &c);
    return !ferror(file);
}

size_t bytes2hex(const unsigned char *bytes, size_t numBytes, char *hex, size_t hexSize)
{
    const unsigned char * bytesStart = bytes;
    const unsigned char * bytesEnd = bytes + numBytes;
    char * hexEnd = hex + hexSize;

    while((hex + 2) < hexEnd && bytes < bytesEnd)
    {
        unsigned char b = *bytes++;
        *hex++ = nibble2hex((b >> 4) & 0x0F);
        *hex++ = nibble2hex(b & 0x0F);
    }
    if (hex < hexEnd)
        *hex++ = '\0';
    return bytes - bytesStart;
}

inline char nibble2hex(unsigned char b)
{
    return (b > 9 ? b - 10 + 'a' : b + '0');
}

int upload(t_en_analyzer *x, char *filename)
{
    CURL *curl_handle;
    CURLcode res;
    struct curl_httppost *formpost = NULL;
    struct curl_httppost *lastptr = NULL;
    struct curl_slist *headerlist = NULL;
    static const char buf[] = "Expect:";
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);  /* will be grown as needed by the realloc above */ 
    chunk.size = 0;    /* no data at this point */
    char url[] = "http://developer.echonest.com/api/v4/track/upload";
    struct json_object *response_obj;
    int response_code;
    char *track_id;

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();
    /* Fill in the file upload field */ 
    curl_formadd(&formpost,
               &lastptr,
               CURLFORM_COPYNAME, "track",
               CURLFORM_FILE, filename,
               CURLFORM_END);
    /* Fill in the submit field too, even if this is rarely needed */ 
    curl_formadd(&formpost,
               &lastptr,
               CURLFORM_COPYNAME, "api_key",
               CURLFORM_COPYCONTENTS, x->api_key->s_name,
               CURLFORM_END);
    /* Fill in the submit field too, even if this is rarely needed */ 
    curl_formadd(&formpost,
               &lastptr,
               CURLFORM_COPYNAME, "filetype",
               CURLFORM_COPYCONTENTS, "wav",
               CURLFORM_END);
    /* Fill in the submit field too, even if this is rarely needed */ 
    curl_formadd(&formpost,
               &lastptr,
               CURLFORM_COPYNAME, "wait",
               CURLFORM_COPYCONTENTS, "true",
               CURLFORM_END);
    headerlist = curl_slist_append(headerlist, buf);
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPPOST, formpost);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    res = curl_easy_perform(curl_handle);
    
    curl_easy_cleanup(curl_handle);
    curl_formfree(formpost);
    curl_slist_free_all (headerlist);
    
    /* check status of api response */
    response_code = check_status(chunk.memory);
    if (response_code != 0) {
        x->track_id = "";
    }
    else {
        response_obj = json_tokener_parse(chunk.memory);
        /* store the track id as an attribute */
        track_id = get_track_id(response_obj);
        x->track_id = track_id;
    }
    return response_code;
}

static size_t WriteMemoryCallback(void *ptr, size_t size, size_t nmemb, void *data)
{
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)data;

    mem->memory = realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory == NULL) {
    /* out of memory! */ 
    printf("not enough memory (realloc returned NULL)\n");
    exit(EXIT_FAILURE);
    }

    memcpy(&(mem->memory[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

char *get_track_id(struct json_object *response_obj)
{
    const char *track_id;
    
    response_obj = json_object_object_get(response_obj, "response");
    response_obj = json_object_object_get(response_obj, "track");
    response_obj = json_object_object_get(response_obj, "id");
    track_id = json_object_to_json_string(response_obj);
    track_id = replace(track_id, "\"", "");
    return track_id;
}

int get_analysis(t_en_analyzer *x, char *param_name, char *param)
{
    char *analysis_url, *analysis_response;
    struct json_object *analysis_obj, *value_obj;
    
    analysis_url = get_analysis_url(x, param_name, param);
    if (analysis_url == "") {
        return 0;
    }
    else {
        analysis_response = get_and_parse_analysis(x, analysis_url);
        if (analysis_response == "") {
            return 0;
        }
        else {
            analysis_obj = json_tokener_parse(analysis_response);
        
            x->bars = json_object_object_get(analysis_obj, "bars");
            x->n_bars = json_object_array_length(x->bars);
            x->beats = json_object_object_get(analysis_obj, "beats");
            x->n_beats = json_object_array_length(x->beats);
            x->sections = json_object_object_get(analysis_obj, "sections");
            x->n_sections = json_object_array_length(x->sections);
            x->segments = json_object_object_get(analysis_obj, "segments");
            x->n_segments = json_object_array_length(x->segments);
            x->tatums = json_object_object_get(analysis_obj, "tatums");
            x->n_tatums = json_object_array_length(x->tatums);
        
            analysis_obj = json_object_object_get(analysis_obj, "track");
        
            value_obj = json_object_object_get(analysis_obj, "tempo");
            x->tempo = json_object_get_double(value_obj);
            value_obj = json_object_object_get(analysis_obj, "time_signature");
            x->time_signature = json_object_get_double(value_obj);
            value_obj = json_object_object_get(analysis_obj, "key");
            x->key = json_object_get_double(value_obj);
            value_obj = json_object_object_get(analysis_obj, "mode");
            x->mode = json_object_get_double(value_obj);
            value_obj = json_object_object_get(analysis_obj, "loudness");
            x->loudness = json_object_get_double(value_obj);
            value_obj = json_object_object_get(analysis_obj, "end_of_fade_in");
            x->end_of_fade_in = json_object_get_double(value_obj);
            value_obj = json_object_object_get(analysis_obj, "start_of_fade_out");
            x->start_of_fade_out = json_object_get_double(value_obj);
        
            return 1;
        }
    }
}

char *get_analysis_url(t_en_analyzer *x, char *param_name, char *param)
{
    char url[1024];
    char *response, *analysis_url, *analysis_status;
    struct json_object *response_obj, *analysis_status_obj;
    int response_code;
    
    param_name = replace(param_name, "\"", "");
    strcpy(url, "http://developer.echonest.com/api/v4/track/profile?format=json&bucket=audio_summary");
    strcat(url, "&api_key=");
    strcat(url, x->api_key->s_name);
    strcat(url, "&");
    strcat(url, param_name);
    strcat(url, "=");
    strcat(url, param);
    
    post("%s", url);
    response = open_url(x, url);

    /* check status of api response */
    response_code = check_status(response);
    if (response_code != 0) {
        analysis_url = "";
    }
    else {
        response_obj = json_tokener_parse(response);
        analysis_status_obj = json_object_object_get(response_obj, "response");
        analysis_status_obj = json_object_object_get(analysis_status_obj, "track");
        analysis_status_obj = json_object_object_get(analysis_status_obj, "status");
        analysis_status = json_object_to_json_string(analysis_status_obj);
        analysis_status = replace(analysis_status, "\"", "");
        if (strcmp(analysis_status, "complete") != 0) {
            post("Analysis status: %s", analysis_status);
            analysis_url = "";
        }
        else {
            analysis_url = extract_analysis_url_from_response(response_obj);
        }
    }
    return analysis_url;
}

int check_status(char *response)
{
    struct json_object *response_obj, *status_obj, *code_obj;
    int response_length, response_code;
    char *message;
    
    /* first, check that it's a valid api response */
    response_length = strlen(response);
    if (response_length < 100) {
        post("Couldn't connect to The Echo Nest API. Are you connected to the internet?");
        return 1;
    }
    
    response_obj = json_tokener_parse(response);
    status_obj = json_object_object_get(response_obj, "response");
    status_obj = json_object_object_get(status_obj, "status");
    code_obj = json_object_object_get(status_obj, "code");
    response_code = json_object_get_int(code_obj);
    status_obj = json_object_object_get(status_obj, "message");
    message = json_object_to_json_string(status_obj);
    if (response_code != 0) {
        post("Error %d: %s", response_code, message);
    }
    return response_code;
}

char *extract_analysis_url_from_response(struct json_object *response_obj)
{
    char *analysis_url;
    
    response_obj = json_object_object_get(response_obj, "response");
    response_obj = json_object_object_get(response_obj, "track");
    response_obj = json_object_object_get(response_obj, "audio_summary");
    response_obj = json_object_object_get(response_obj, "analysis_url");
    analysis_url = json_object_to_json_string(response_obj);
    /* get rid of escaped backslashes and quotation marks in url */
    analysis_url = replace(analysis_url, "\\/", "/");
    analysis_url = replace(analysis_url, "\"", "");
    return analysis_url;
}

char *replace(const char *src, const char *from, const char *to)
{
    /* from http://www.daniweb.com/code/snippet216517.html */
    size_t size    = strlen(src) + 1;
    size_t fromlen = strlen(from);
    size_t tolen   = strlen(to);
    char *value = malloc(size);
    char *dst = value;
    if ( value != NULL )
    {
        for ( ;; )
        {
            const char *match = strstr(src, from);
            if ( match != NULL )
            {
                size_t count = match - src;
                char *temp;
                size += tolen - fromlen;
                temp = realloc(value, size);
                if ( temp == NULL )
                {
                    free(value);
                    return NULL;
                }
                dst = temp + (dst - value);
                value = temp;
                memmove(dst, src, count);
                src += count;
                dst += count;
                memmove(dst, to, tolen);
                src += fromlen;
                dst += tolen;
            }
            else
            {
                strcpy(dst, src);
                break;
            }
        }
    }
    return value;
}

char *get_and_parse_analysis(t_en_analyzer *x, char *analysis_url)
{
    char *response;
    int val;
    
    response = open_url(x, analysis_url);
    val = strstr(response, "AccessDenied");
    if (val > 0) {
        post("Access Denied. Your internet may be too slow");
        return "";
    }
    else {
        return response;
    }
}

char *open_url(t_en_analyzer *x, char *analysis_url)
{
    CURL *curl_handle;
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();
    curl_easy_setopt(curl_handle, CURLOPT_URL, analysis_url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
    curl_easy_perform(curl_handle);
    curl_easy_cleanup(curl_handle);
    
    return chunk.memory;
}

void en_analyzer_segment(t_en_analyzer *x, t_symbol *s, long argc, t_atom *argv)
{
    /* send data about segment n out the various outlets */
    long n;
    struct json_object *seg_obj;
    struct json_object *obj, *array_obj; // these get reused
    float loudness_start, loudness_max_time, loudness_max;
    float start, duration;
    float val; // this gets reused 
    short i;
    t_atom loudnesses[3];
    t_atom start_dur[2];
    t_atom pitches[12];
    t_atom timbre[12];
    
    n = atom_getlong(argv);
    
    if (n < 1 || n > x->n_segments) {
        post("There is no segment with index %d", n);
        return;
    }
    
    seg_obj = json_object_array_get_idx(x->segments, n - 1);

    obj = json_object_object_get(seg_obj, "timbre");
    for (i=0; i<12; i++) {
        array_obj = json_object_array_get_idx(obj, i);
        val = json_object_get_double(array_obj);
        atom_setfloat(timbre + i, val);
    }
    outlet_list(x->b_outlet4, NULL, 12, timbre);
        
    obj = json_object_object_get(seg_obj, "pitches");
    for (i=0; i<12; i++) {
        array_obj = json_object_array_get_idx(obj, i);
        val = json_object_get_double(array_obj);
        atom_setfloat(pitches + i, val);
    }
    outlet_list(x->b_outlet3, NULL, 12, pitches);
        
    obj = json_object_object_get(seg_obj, "loudness_start");
    loudness_start = json_object_get_double(obj);
    obj = json_object_object_get(seg_obj, "loudness_max_time");
    loudness_max_time = json_object_get_double(obj);
    obj = json_object_object_get(seg_obj, "loudness_max");
    loudness_max = json_object_get_double(obj);
    atom_setfloat(loudnesses, loudness_start);
    atom_setfloat(loudnesses + 1, loudness_max_time);
    atom_setfloat(loudnesses + 2, loudness_max);
    outlet_list(x->b_outlet2, NULL, 3, loudnesses);
    
    obj = json_object_object_get(seg_obj, "start");
    start = json_object_get_double(obj);
    obj = json_object_object_get(seg_obj, "duration");
    duration = json_object_get_double(obj);
    atom_setfloat(start_dur, start);
    atom_setfloat(start_dur + 1, duration);
    outlet_list(x->b_outlet1, NULL, 2, start_dur);
}

void en_analyzer_bar(t_en_analyzer *x, t_symbol *s, long argc, t_atom *argv)
{
    long n;
    struct json_object *bar_obj;
    
    n = atom_getlong(argv);
    if (n < 1 || n > x->n_bars) {
        post("There is no bar with index %d", n);
        return;
    }
    bar_obj = json_object_array_get_idx(x->bars, n - 1);
    en_analyzer_quantum(x, bar_obj, n);
}

void en_analyzer_beat(t_en_analyzer *x, t_symbol *s, long argc, t_atom *argv)
{
    long n;
    struct json_object *beat_obj;
    
    n = atom_getlong(argv);
    if (n < 1 || n > x->n_beats) {
        post("There is no beat with index %d", n);
        return;
    }
    beat_obj = json_object_array_get_idx(x->beats, n - 1);
    en_analyzer_quantum(x, beat_obj, n);
}

void en_analyzer_section(t_en_analyzer *x, t_symbol *s, long argc, t_atom *argv)
{
    long n;
    struct json_object *section_obj;
    
    n = atom_getlong(argv);
    if (n < 1 || n > x->n_sections) {
        post("There is no section with index %d", n);
        return;
    }
    section_obj = json_object_array_get_idx(x->sections, n - 1);
    en_analyzer_quantum(x, section_obj, n);
}

void en_analyzer_tatum(t_en_analyzer *x, t_symbol *s, long argc, t_atom *argv)
{
    long n;
    struct json_object *tatum_obj;
    
    n = atom_getlong(argv);
    if (n < 1 || n > x->n_tatums) {
        post("There is no tatum with index %d", n);
        return;
    }
    tatum_obj = json_object_array_get_idx(x->tatums, n - 1);
    en_analyzer_quantum(x, tatum_obj, n);
}

void en_analyzer_quantum(t_en_analyzer *x, struct json_object *quantum_obj, long n)
{
    struct json_object *obj;
    float start, duration;
    t_atom start_dur[2];
    
    obj = json_object_object_get(quantum_obj, "start");
    start = json_object_get_double(obj);
    obj = json_object_object_get(quantum_obj, "duration");
    duration = json_object_get_double(obj);
    atom_setfloat(start_dur, start);
    atom_setfloat(start_dur + 1, duration);
    outlet_list(x->b_outlet1, NULL, 2, start_dur);
}

void en_analyzer_global(t_en_analyzer *x)
{
    t_atom global_features[7];
    t_atom num_quanta[5];
    
    atom_setlong(num_quanta, x->n_sections);
    atom_setlong(num_quanta + 1, x->n_bars);
    atom_setlong(num_quanta + 2, x->n_beats);
    atom_setlong(num_quanta + 3, x->n_tatums);
    atom_setlong(num_quanta + 4, x->n_segments);
    outlet_list(x->b_outlet6, NULL, 5, num_quanta);
    
    atom_setfloat(global_features, x->tempo);
    atom_setfloat(global_features + 1, x->time_signature);
    atom_setfloat(global_features + 2, x->key);
    atom_setfloat(global_features + 3, x->mode);
    atom_setfloat(global_features + 4, x->loudness);
    atom_setfloat(global_features + 5, x->end_of_fade_in);
    atom_setfloat(global_features + 6, x->start_of_fade_out);
    outlet_list(x->b_outlet5, NULL, 7, global_features);
}

void *en_analyzer_new(t_symbol *s, long argc, t_atom *argv)
{
	t_en_analyzer *x = ((t_en_analyzer *)object_alloc(en_analyzer_class));
    t_symbol *buf = 0;
    dsp_setup((t_pxobject *)x, 1);
	buf = atom_getsymarg(0, argc, argv);
	x->api_key = atom_getsymarg(1, argc, argv);
	
    x->b_name = buf;
    x->b_buf = 0;
    x->b_msr = sys_getsr() * 0.001;
    x->n_bars = 0;
    x->n_beats = 0;
    x->n_sections = 0;
    x->n_segments = 0;
    x->n_tatums = 0;
    x->tempo = 0;
    x->time_signature = 0;
    x->key = 0;
    x->loudness = 0;
    x->end_of_fade_in = 0;
    x->start_of_fade_out = 0;
	
	x->b_outlet6 = outlet_new((t_object *)x, NULL);
	x->b_outlet5 = outlet_new((t_object *)x, NULL);
	x->b_outlet4 = outlet_new((t_object *)x, NULL);
	x->b_outlet3 = outlet_new((t_object *)x, NULL);
	x->b_outlet2 = outlet_new((t_object *)x, NULL);
	x->b_outlet1 = outlet_new((t_object *)x, NULL);
	return (x);
}
