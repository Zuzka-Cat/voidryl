#include "client.c"
#include "models.c"
#include "networking.c"
#include "server.c"
#include "shared.c"

static void print_usage(String program) {
  fprintf(stderr, "usage: %.*s --server [port]\n", (int)program.length,
          (char *)program.items);
  fprintf(stderr,
          "       %.*s --client <ipv4|localhost> <port> <five-letter-name>\n",
          (int)program.length, (char *)program.items);
}

int main(int argc, char **argv) {
  u16 port;
  u8 name_storage[PLAYER_NAME_LENGTH];
  String name_out = {name_storage, 0, PLAYER_NAME_LENGTH};
  if (argc >= 2 && string_eq(string_from_cstr(argv[1]), STR("--server"))) {
    port = 4242;
    if (argc >= 3 && !valid_port(string_from_cstr(argv[2]), &port)) {
      print_usage(string_from_cstr(argv[0]));
      return 1;
    }
    server_run(port);
    return 0;
  }
  if (argc == 5 && string_eq(string_from_cstr(argv[1]), STR("--client"))) {
    String host = string_from_cstr(argv[2]);
    String port_text = string_from_cstr(argv[3]);
    String name_in = string_from_cstr(argv[4]);
    if (!valid_port(port_text, &port) ||
        !valid_player_name(name_in, &name_out)) {
      print_usage(string_from_cstr(argv[0]));
      return 1;
    }
    return client_run(host, port, name_out);
  }
  print_usage(string_from_cstr(argv[0]));
  return 1;
}
