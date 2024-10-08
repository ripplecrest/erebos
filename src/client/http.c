#include "http.h"
#include "sock.h"
#include <assert.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <time.h>

const char *CONTENT_LENGTH_HEADER = "Content-Length: ";
const char *GET_REQ_TEMPLATE =
    "GET %s HTTP/1.1\r\nConnection: keep-alive\r\n\r\n";
const char *POST_REQ_TEMPLATE = "POST %s HTTP/1.1\r\nContent-Type: "
                                "%s\r\nContent-Length: %d\r\n%s\r\n\r\n";

/* Log a http_res_t */
void print_http_res(const http_res_t res) {
  printf("--[ STATUS CODE: %i ]--\n", res.status_code);
  printf("--[ REQUEST ]--\n%s\n--[ REQUEST ]--\n", res.request);
  printf("%s\n", res.data);
  puts("--[ END ]--\n");
}

/* Parse HTTP status code */
long parse_http_status_code(const char *buf) {
  const char *status_code_start;
  char *endptr;
  long status_code;

  status_code_start = strstr(buf, " ") + 1;
  if (status_code_start == NULL) {
    return -HTTP_INVALID_RESPONSE;
  }
  status_code = strtol(status_code_start, &endptr, 10);
  if (endptr == status_code_start) {
    return -HTTP_INVALID_RESPONSE;
  }
  return status_code;
}

/* Parse HTTP content length header */
long parse_http_content_length(const char *buf) {
  const char *content_length_start;
  char *endptr;
  long content_length;

  content_length_start =
      strstr(buf, CONTENT_LENGTH_HEADER) + strlen(CONTENT_LENGTH_HEADER);

  content_length = strtol(content_length_start, &endptr, 10);
  if (endptr == content_length_start) {
    return -HTTP_INVALID_RESPONSE;
  }
  return content_length;
}

/* Parse HTTP response body */
int parse_http_body(int sfd, char *src, char *dest, long content_length, long total_bytes) {
  const char *body_start;
  long header_length, received_length, left_length;
 
  body_start = strstr(src, "\r\n\r\n");
  if (!body_start) {
    perror("Header delimeter not found\n");
    return -HTTP_INVALID_RESPONSE;
  }
  body_start += 4;

  header_length = body_start - src;

  received_length = MIN(total_bytes - header_length, content_length);
  if (received_length < 0) { // in case God is against us
    perror("Received length is negative\n");
    return -HTTP_SOCKET_ERR;
  }
  memcpy(dest, body_start, received_length);

  if (header_length + content_length > total_bytes) {
    left_length = content_length - received_length;
    if (recv_response(sfd, dest + received_length, left_length) < 0) {
      perror("Failed to receive left over data\n");
      return -HTTP_SOCKET_ERR;
    }
  }
  
  return 0;
}

int http_post(int sfd, const char *path,
              const char *content_type, const char *parameters,
              http_res_t *res) {
  long total_bytes, bytes_read, content_length, status_code;
  size_t req_buf_len;
  char req_buffer[HTTP_BUFFER_SIZE];
  char buffer[HTTP_BUFFER_SIZE];

  snprintf(req_buffer, HTTP_BUFFER_SIZE - 1, POST_REQ_TEMPLATE, path, content_type,
           strlen(parameters), parameters);
  req_buf_len = strlen(req_buffer);

  res->request = malloc(req_buf_len);
  if (res->request == NULL) {
    free(res->data); // free previously allocated data
    return -HTTP_OOM;
  }
  strncpy(res->request, req_buffer, req_buf_len - 1);
  
  if (send_request(sfd, req_buffer) < 0) {
    perror("Error: failed to send request\n");
    return -HTTP_SOCKET_ERR;
  }

  if (HTTP_VERBOSE)
    puts("Sent POST request");

  /* Receive response from server */
  total_bytes = 0;
  while ((bytes_read = recv(sfd, buffer + total_bytes,
                            HTTP_BUFFER_SIZE - 1 - total_bytes, 0)) > 0) {
    total_bytes += bytes_read;
    // add temporary null terminator
    buffer[total_bytes] = 0;
    if (NULL != strstr(buffer + total_bytes - bytes_read, "\r\n\r\n")) {
      // if we read all headers stop reading
      break;
    }

    if (total_bytes >= HTTP_BUFFER_SIZE - 1) {
      buffer[HTTP_BUFFER_SIZE - 1] = 0;
      break;
    }
  }

  /* Check if response starts with "HTTP" */
  if (memcmp(buffer, "HTTP", 4)) {
    return -HTTP_INVALID_RESPONSE;
  }

  /* Parse status code */
  status_code = parse_http_status_code(buffer);
  if (status_code < 0) {
    return -HTTP_INVALID_RESPONSE;
  }
  res->status_code = (int)status_code;

  /* Parse content length */
  content_length = parse_http_content_length(buffer);
  if (content_length < 0) {
    return -HTTP_INVALID_RESPONSE;
  }
  res->size = (size_t)content_length;

  /* Parse the response body */
  res->data = malloc(res->size);
  if (res->data == NULL) {
    return -HTTP_OOM;
  }

  if (parse_http_body(sfd, buffer, res->data, content_length, total_bytes)) {
    return -HTTP_INVALID_RESPONSE;
  }
  
  if (HTTP_VERBOSE)
    puts("Parsed response");
  if (HTTP_VERBOSE > 1)
    print_http_res(*res);

  return HTTP_SUCCESS;
}

int http_get(int sfd, const char *path, http_res_t *res) {
  char request_buf[HTTP_BUFFER_SIZE]; // use separate buffer for the request
  char buf[HTTP_BUFFER_SIZE];

  int bytes_read;
  long total_bytes, status_code, content_length;
  size_t req_buf_len;

  /* send request */
  snprintf(request_buf, HTTP_BUFFER_SIZE - 1, GET_REQ_TEMPLATE, path);
  req_buf_len = strlen(request_buf);

  if (send_request(sfd, request_buf) < 0) {
    perror("Error: failed to send request\n");
    return -HTTP_SOCKET_ERR;
  }

  if (HTTP_VERBOSE)
    puts("Sent GET request");

  res->request = malloc(req_buf_len);
  if (res->request == NULL) {
    free(res->data); // free previously allocated data
    return -HTTP_OOM;
  }
  strncpy(res->request, request_buf, req_buf_len - 1);

  /* receive response from server */
  total_bytes = 0;
  while ((bytes_read = recv(sfd, buf + total_bytes,
                            HTTP_BUFFER_SIZE - 1 - total_bytes, 0)) > 0) {
    total_bytes += bytes_read;
    // add temporary null terminator
    buf[total_bytes] = 0;
    if (strstr(buf + total_bytes - bytes_read, "\r\n\r\n")) {
      // if we read all headers stop reading
      break;
    }

    if (total_bytes >= HTTP_BUFFER_SIZE - 1) {
      buf[HTTP_BUFFER_SIZE - 1] = 0;
      break;
    }
  }
  if (HTTP_VERBOSE)
    puts("Received data from server");

  /* Check if response starts with "HTTP" */
  if (memcmp(buf, "HTTP", 4)) {
    return -HTTP_INVALID_RESPONSE;
  }

  /* Parse status code */
  status_code = parse_http_status_code(buf);
  if (status_code < 0) {
    return -HTTP_INVALID_RESPONSE;
  }
  res->status_code = (int)status_code;

  /* Parse content length */
  content_length = parse_http_content_length(buf);
  if (content_length < 0) {
    return -HTTP_INVALID_RESPONSE;
  }
  res->size = (size_t)content_length;

  /* Parse the response body */
  res->data = malloc(res->size);
  if (res->data == NULL) {
    return -HTTP_OOM;
  }

  if (parse_http_body(sfd, buf, res->data, content_length, total_bytes) < 0) {
    return -HTTP_INVALID_RESPONSE;
  }

  if (HTTP_VERBOSE)
    puts("Parsed response");
  if (HTTP_VERBOSE > 1)
    print_http_res(*res);

  return HTTP_SUCCESS;
}

/* Perform a GET request to path and write the body to the file specified in
 * f_path */
int http_download_data_to_file(int sfd, const char *path, const char *f_path) {
  http_res_t res;
  FILE *file;
  int error;

  error = http_get(sfd, path, &res);
  if (error != HTTP_SUCCESS) {
    return error;
  }

  file = fopen(f_path, "w");
  if (file == NULL) {
    perror("Error: Failed to open file");
    http_free(&res);
    return -1;
  }

  if (fwrite(res.data, sizeof(char), res.size, file) != res.size) {
    perror("Error: Failed to write data to file");
    fclose(file);
    http_free(&res);
    return -2;      
  }

  if (fclose(file) != 0) {
    perror("Error: Failed to close file");
    http_free(&res);
    return -3;
  }

  http_free(&res);
  return 0;
}

/* Properly free a http_res_t structure */
void http_free(http_res_t *res) {
  free(res->data);
  res->data = NULL;
  free(res->request);
  res->request = NULL;

  res->size = 0;
  res->status_code = 0;
  return;

}
