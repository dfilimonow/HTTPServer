#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <string>
#include <map>
#include <sstream>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <cctype>
#include <regex>

#define LINE_SIZE 9999

void syserr(const char *fmt, ...) {
  va_list fmt_args;
  int err;

  fprintf(stderr, "ERROR: ");
  err = errno;

  va_start(fmt_args, fmt);
  if (vfprintf(stderr, fmt, fmt_args) < 0) {
    fprintf(stderr, " (also error in syserr) ");
  }
  va_end(fmt_args);
  fprintf(stderr, " (%d; %s)\n", err, strerror(err));
  exit(EXIT_FAILURE);
}

//Checking file existance and path location(dependent on flag)
bool does_exist(const std::string &s, const std::string &root, struct stat &buffer, int flag)
{
  if (flag)
  {
    std::string root_path = std::filesystem::path(std::filesystem::absolute(root)).lexically_normal();
    std::string file_path = std::filesystem::path(std::filesystem::absolute(s)).lexically_normal();
    if (file_path.substr(0, root_path.size()) != root_path)
    {
      return false;
    }
  }

  return (stat(s.c_str(), &buffer) == 0);
}

//Validating whole request
bool validate_request(const std::string &s, std::smatch &result, const std::regex &reg)
{
  return regex_search(s, result, reg);
}

//Sending error respond
void send_error_respond(int socket, int status_code, const std::string &reason_phrase)
{
  std::string response = "HTTP/1.1 " + std::to_string(status_code) + " " + reason_phrase + "\r\n";

  if (status_code == 501 || status_code == 400)
  {
    response.append("Connection:close\r\n");
  }
  response.append("\r\n");

  if (send(socket, response.c_str(), strlen(response.c_str()), 0) < 0)
  {
    syserr("writing on stream socket");
  }
}

//Sending file in corelated server espond
void send_corelated_respond(int socket, const std::map<std::string,
                                                       std::pair<std::string, std::string> >::iterator &it)
{
  std::string response = "HTTP/1.1 " + std::to_string(302) + " " + "Another server" + "\r\n" +
                         "Location:http://" + it->second.first + ":" + it->second.second + it->first + "\r\n\r\n";
  if (send(socket, response.c_str(), strlen(response.c_str()), 0) < 0)
  {
    syserr("writing on stream socket");
  }
}

//Sending success request respond
bool send_good_respond(int socket, std::ifstream &file_path, bool should_add_body)
{
  std::stringstream body;
  body << file_path.rdbuf();

  std::string response = "HTTP/1.1 " + std::to_string(200) + " " + "Success" + "\r\n" +
                         "Server:WadowickiSerwer\r\n" +
                         "Content-Type:application/octet-stream\r\n" +
                         "Content-Length:" + std::to_string(body.str().size()) + "\r\n" +
                         "\r\n";

  if (should_add_body)
  {
    response.append(body.str());
  }

  const char *response_buffer = &response[0];
  ssize_t written_now = 0, written = 0;
  while (response.length() - written)
  {
    written_now = send(socket, response_buffer, (response.length() - written), 0);
    if (written_now < 0)
    {
      syserr("Problem");
      file_path.close();
      return 0;
    }
    response_buffer += written_now;
    written += written_now;
  }

  file_path.close();
  return 1;
}

//Checking whether we should close socket
void to_close(const std::string &request, const std::regex &header, std::string &connection_status)
{
  std::smatch result;
  regex_search(request, result, header);

  std::string::const_iterator search_start(request.cbegin());
  while (regex_search(search_start, request.cend(), result, header))
  {
    std::string field_name = result[1];
    std::transform(field_name.begin(), field_name.end(), field_name.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (field_name == "connection")
    {
      connection_status = result[2];
      break;
    }
    search_start = result.suffix().first;
  }
}

std::map<std::string, std::pair<std::string, std::string> > corelated_resources;

int main(int argc, char *argv[])
{
  int port_num;
  // Regular expressions
  std::regex request("((GET|HEAD|[^ ]+) ([[:alnum:]\\.\\-/]+) HTTP/1.1\r\n)(([[:alpha:]\\-]+):[ ]*(.+)\r\n)*(\r\n)(.*)");
  std::regex bad_resource_request("((GET|HEAD|[^ ]+) ([\\S]+) HTTP/1.1\r\n)(([[:alpha:]\\-]+):[ ]*(.+)\r\n)*(\r\n)(.*)");
  std::regex header("([[:alpha:]\\-]+):[ ]*(.+)\r\n");
  std::regex corelated("([[:alnum:].\\-\\/]+)\t(\\d+\\.\\d+\\.\\d+\\.\\d+)\t(\\d+)");

  // Setting up a port
  if (argc == 3)
    port_num = 8080;
  else if (argc == 4)
    port_num = std::atoi(argv[3]);
  else
  {
    return EXIT_FAILURE;
  }

  //Checking given paths
  std::string path = argv[1];
  struct stat s;
  if (!does_exist(path, path, s, 0) || !S_ISDIR(s.st_mode))
  {
    return EXIT_FAILURE;
  }

  std::string file = argv[2];
  if (!does_exist(file, path, s, 0) || !S_ISREG(s.st_mode))
  {
    return EXIT_FAILURE;
  }

  std::smatch line_tokens;
  std::ifstream ifs(file);
  //Creating map with corelated resources(resources on corelated servers)
  if (ifs.is_open())
  {
    for (std::string line; std::getline(ifs, line);)
    {
      std::regex_search(line, line_tokens, corelated);
      if (corelated_resources.find(line_tokens[1]) == corelated_resources.end())
      {
        corelated_resources.insert(std::make_pair(line_tokens[1], std::make_pair(line_tokens[2], line_tokens[3])));
      }
    }
  }
  else
  {
    return EXIT_FAILURE;
  }

  int ear, rc;
  struct sockaddr_in server;

  // Creating socket
  ear = socket(PF_INET, SOCK_STREAM, 0);
  if (ear == -1)
    syserr("socket");

  // Creating and listening to server
  server.sin_family = AF_INET;
  server.sin_addr.s_addr = htonl(INADDR_ANY);
  server.sin_port = ntohs(port_num);
  rc = bind(ear, (struct sockaddr *)&server, sizeof(server));
  if (rc == -1)
    syserr("bind");

  rc = listen(ear, 100);
  if (rc == -1)
    syserr("listen");

  struct timeval timeout;
  //Work
  while (true)
  {
    int msgsock, ret, input_length;
    int *con;
    pthread_t t;
    std::string buffer;
    char line[LINE_SIZE + 1];
    bool socket_status = true, validation_status = false;

    //Accepting connection
    msgsock = accept(ear, (struct sockaddr *)NULL, NULL);
    if (msgsock == -1)
    {
      syserr("accept");
    }

    while (true)
    {
      //Opening socket to read from it
      auto socket = fdopen(msgsock, "r");
      buffer.clear();
      char *input_buffer = NULL;
      int line_nr = 0;
      bool last = true, should_end = 0;
      size_t buf_length = 0;
      int connection = 0, type = 0, length = 0, server = 0;
      for (;;)
      {
        std::smatch temp_result;
        std::string tempbuffer;
        char *input_buffer = NULL;
        size_t buf_length = 0;
        line_nr++;
        //Reading one line from socket
        ret = getline(&input_buffer, &buf_length, socket);

        //Line to big is an error
        if (ret > 8300)
        {
          send_error_respond(msgsock, 404, "Too big line");
          should_end = true;
          break;
        }

        //After \r\n\r\n we end reading
        if (ret <= 0 || ret == 2 && input_buffer[0] == '\r' && input_buffer[1] == '\n' && last)
        {
          break;
        }

        if (input_buffer[ret - 2] == '\r' && input_buffer[ret - 1] == '\n')
        {
          last = true;
        }
        else
        {
          last = false;
        }

        //Appending to one line buffer
        tempbuffer.append(input_buffer);
        tempbuffer.append("\r\n");

        //Parsing header input, regular expression will not have to deal with header problem
        if (line_nr >= 2 && !validate_request(tempbuffer, temp_result, header))
        {
          send_error_respond(msgsock, 400, "Bad header");
          should_end = true;
          break;
        }
        else
        {
          if(line_nr >= 2 && temp_result.size() >= 3) {
            std::string field_name = temp_result[1];
            std::transform(field_name.begin(), field_name.end(), field_name.begin(),
                          [](unsigned char c) { return std::tolower(c); });
            if (field_name == "content-length")
            {
              if (std::atoi(temp_result[2].str().c_str()))
              {
                //Body is not allowed
                send_error_respond(msgsock, 400, "body");
                should_end = true;
                break;
              }
              length++;
            }
            else if (field_name == "server")
            {
              server++;
            }
            else if (field_name == "content-type")
            {
              type++;
            }
            else if (field_name == "connection")
            {
              connection++;
            }

            //Duplicated are not allowed
            if (connection > 1 || type > 1 || server > 1 || length > 1)
            {
              send_error_respond(msgsock, 400, "Duplicated");
              should_end = true;
              break;
            }
          }
        }
        //Appending to main buffer
        buffer.append(input_buffer);
        if (last)
        {
          buffer.append("\r\n");
        }
        free(input_buffer);
      }
      //Maybe we sent something bad
      if (should_end)
      {
        break;
      }

      std::smatch result;
      std::string connection_status = "";
      //Check if buffer matches regex
      if (validate_request(buffer, result, request))
      {
        validation_status = true;
        std::cout << buffer << std::endl;
        std::string method = result.str(2);
        std::string request_target = result.str(3);
        //Check methods
        if (result.str(2) == "GET" || result.str(2) == "HEAD")
        {
          //Check if resource begins with slash
          if (request_target[0] == '/')
          {
            std::string resource = path + request_target;
            struct stat st;
            //Checking path existance
            if (!does_exist(resource, path, st, 1))
            {
              auto it = corelated_resources.find(request_target);
              //When path doesnt exist we check its existance in corelated servers
              if (it != corelated_resources.end())
              {
                send_corelated_respond(msgsock, it);
              }
              else
              {
                send_error_respond(msgsock, 404, "Resource does not exist");
              }
            }
            else if (!S_ISREG(st.st_mode))
            {
              //Resource must be a file
              send_error_respond(msgsock, 404, "Bad resource");
            }
            else
            {
              std::ifstream file_path(resource);
              //Trying to open a file
              if (file_path.is_open())
              {
                //Sending good response
                bool check = send_good_respond(msgsock, file_path, (result.str(2) == "GET"));
                to_close(buffer, header, connection_status);
                if (connection_status == "close" || !check)
                {
                  break;
                }
              }
              else
              {
                //Could not open file
                send_error_respond(msgsock, 400, "Cannot open file");
                break;
              }
            }
          }
          else
          {
            //Without slash resource is bad
            send_error_respond(msgsock, 400, "Bad resource");
            break;
          }
        }
        else
        {
          //Given method is not avaiable
          send_error_respond(msgsock, 501, "Method not available");
          break;
        }
        buffer.clear();
      }
      else
      {
        //Validation did not suceed, we must check if resource was appropriate to send proper code
        if (validate_request(buffer, result, bad_resource_request))
        {
          send_error_respond(msgsock, 404, "Bad resource");
        }
        else
        {
          send_error_respond(msgsock, 400, "Bad request");
          break;
        }
      }
    };

    if (close(msgsock) < 0)
    {
      syserr("close");
    }
  }
}
