#define MAGIC_INITIALIZE 0xC801FFFF
#define MAGIC_OUTPUT_VOLTAGE 0xC824FFFF
#define MAGIC_INPUT_VOLTAGE 0xC846FFFF
#define MAGIC_SET_OUTPUT 0xC842FFFF
#define MAGIC_GET_ID 0xC841FFFF

#define MAGIC_UNKNOWN_02 0xC802FFFF
#define MAGIC_UNKNOWN_23 0xC823FFFF
#define MAGIC_UNKNOWN_45 0xC845FFFF


// Array of commands to poll in round-robin fashion
uint32_t pollCommands[] = {
  MAGIC_OUTPUT_VOLTAGE,
  MAGIC_INPUT_VOLTAGE,
  MAGIC_SET_OUTPUT,
  MAGIC_GET_ID,
  MAGIC_UNKNOWN_02,
  MAGIC_UNKNOWN_23,
  MAGIC_UNKNOWN_45
};

int commandIndex = 0;
const int numCommands = sizeof(pollCommands) / sizeof(pollCommands[0]);