/* EA076 - Projeto 2: Datalogger
    Isabella Bigatto    138537
    Júlia Dias          156019

    Objetivo: implementar um dispositivo capaz de armazenar dados em uma memória EEPROM
    e fazer o controle do mesmo de forma remota, a partir de um computador e um teclado matricial.

    Referências e códigos usados:
    - Biblioteca keypad.h: http://playground.arduino.cc/Code/Keypad#Functions
    - Kernel em tempo real: https://github.com/embarcados-unicamp/rt-kernel
    - Comunicação I2C: http://tronixstuff.com/2010/10/20/tutorial-arduino-and-the-i2c-bus/
*/

/* stdio.h contem rotinas para processamento de expressoes regulares */
#include <stdio.h>

/* Bibliotecas referentes ao protocolo I2C, teclado matricial e interrupção periódica, respectivamente */
#include <Wire.h>
#include <Keypad.h>
#include <TimerOne.h>

#define ledPin 2      // LED está ligado ao pino 2 
#define ROWS 4
#define COLUMNS 3

/* As flags abaixo são setadas quando '#', algum número e '*' são apertadas no teclado matricial, respectivamente.
  Elas são usadas no controle das ações que serão executadas no loop principal */

int flag_hashtag = 0, flag_number = 0, flag_star = 0;

/* mode_auto e measure são usadas na função check_modeauto() */
int mode_auto = 0, measure = 0;

/* Flags globais para controle de processos da interrupcao */
volatile int flag_check_command = 0;

/* Processo de bufferizacao. Caracteres recebidos sao armazenados em um buffer. Quando um caractere
    de fim de linha ('\n') e recebido, todos os caracteres do buffer sao processados simultaneamente.
*/

/* Buffer de dados recebidos */
#define MAX_BUFFER_SIZE 30
typedef struct {
  char data[MAX_BUFFER_SIZE];
  unsigned int tam_buffer;
} serial_buffer;

/* Teremos somente um buffer em nosso programa, O modificador volatile
    informa ao compilador que o conteudo de Buffer pode ser modificado a qualquer momento. Isso
    restringe algumas otimizacoes que o compilador possa fazer, evitando inconsistencias em
    algumas situacoes (por exemplo, evitando que ele possa ser modificado em uma rotina de interrupcao
    enquanto esta sendo lido no programa principal).
*/

volatile serial_buffer Buffer;

/* Todas as funcoes a seguir assumem que existe somente um buffer no programa e que ele foi
    declarado como Buffer. Esse padrao de design - assumir que so existe uma instancia de uma
    determinada estrutura - se chama Singleton (ou: uma adaptacao dele para a programacao
    nao-orientada-a-objetos). Ele evita que tenhamos que passar o endereco do
    buffer como parametro em todas as operacoes (isso pode economizar algumas instrucoes PUSH/POP
    nas chamadas de funcao, mas esse nao eh o nosso motivo principal para utiliza-lo), alem de
    garantir um ponto de acesso global a todas as informacoes contidas nele.
*/

/* Rotinas de interrupcao */

/* Rotina auxiliar para comparacao de strings */

int str_cmp(char *s1, char *s2, int len) {
  /* Compara duas strings em relação ao comprimento len. Retorna 1 se têm o mesmo tamanho e 0 se não */
  int i;
  for (i = 0; i < len; i++) {
    if (s1[i] != s2[i]) return 0;
    if (s1[i] == '\0') return 1;
  }
  return 1;
}

/* Limpa buffer */
void buffer_clean() {
  Buffer.tam_buffer = 0;
}

/* Adiciona caractere ao buffer */
int buffer_add(char c_in) {
  if (Buffer.tam_buffer < MAX_BUFFER_SIZE) {
    Buffer.data[Buffer.tam_buffer++] = c_in;
    return 1;
  }
  return 0;
}

/* Ao receber evento da UART */
void serialEvent() {
  char c;
  while (Serial.available()) {
    c = Serial.read();
    if (c == '\n') {
      buffer_add('\0'); /* Se recebeu um fim de linha, coloca um terminador de string no buffer */
      flag_check_command = 1;
    } else {
      buffer_add(c);
    }
  }
}

/* As funções a seguir habilitam o teclado matricial através da biblioteca Keypad.h */

/* char keyMap declara qual caractere corresponde a cada posição da matriz, dependendo do número de linhas e colunas definidos */
char keyMap[ROWS][COLUMNS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

/* Relaciona as linhas e colunas com os pinos digitais do Arduino */
byte rowPins[ROWS] = { 11, 10, 9, 8 };
byte colPins[COLUMNS] = { 7, 6, 5 };

/* Cria o teclado, fazendo a varredura e verificando o caractere apertado */
Keypad keypad = Keypad(makeKeymap(keyMap), rowPins, colPins, ROWS, COLUMNS);

/* Teclado habilitado */

/* Funcoes internas ao void main() */

void setup() {
  
  /* Inicializacao */
  buffer_clean();
  flag_check_command = 0;
  Serial.begin(9600);
  Wire.begin();
  Timer1.initialize(500000);                 //Interrupção a cada 0,5s
  Timer1.attachInterrupt(check_modeauto);   //Associa a função check_modeauto() à interrupção periódica
  pinMode(ledPin, OUTPUT);
  
}

/* Os dois blocos a seguir fazem a comunicação do Arduino com a EEPROM através do protocolo I2C  */

unsigned char EEPROM_read(unsigned char address) {

  unsigned char data;
  Wire.beginTransmission(0x50);     //Arduino "acorda" a EEPROM para iniciar a comunicação, 0x50 é o endereço da memória (ver datasheet)
  Wire.write(address);              //address é o local na memória cujo conteúdo queremos ler
  Wire.endTransmission();
  delay(5);

  Wire.requestFrom(0x50, 1);        //com a comunicação estabelecida anteriormente, mestre requisita o conteúdo do endereço já especificado e 1 é o número de bytes que serão lidos
  data = Wire.read();               //data recebe o valor lido
  delay(5);

  return data;
}

unsigned char EEPROM_write(unsigned char address, unsigned char data) {

  Wire.beginTransmission(0x50);
  //Iniciada a transmissão, o primeiro Wire.write indica o endereço que vamos escrever e o segundo escreve o dado nele
  Wire.write(address);
  Wire.write(data);
  Wire.endTransmission();
  delay(5);

  return 0;

}

/* filesystem_write é um sistema de arquivos que funciona como PILHA,
  para determinar qual é o próximo espaço livre da memória em que será gravado um dado
  - n indica quantos espaços estão ocupados
  - n++ indica o próximo espaço livre que pode ser usado */

unsigned char filesystem_write(unsigned char data) {

  unsigned char n;
  n = EEPROM_read(0);
  n++;
  EEPROM_write(n, data);
  EEPROM_write(0, n);       // atualiza quantos espaços estão ocupados na memória
  delay(5);

  return 0;
}

/*check_modeauto() inicia as medições automáticas a partir de '#3*' do teclado */
void check_modeauto () {
  if (mode_auto == 1) {
    measure = 1;
  } else measure = 0;
}

void loop() {
  char out_buffer[50];
  int flag_write = 0;
  char key = keypad.getKey();   // key recebe o caractere que é apertado no teclado
  byte valueLDR;             // adapta os valores medidos pelo LDR para gravá-lo na memória

  /* Controle do TECLADO MATRICIAL
    - #1*: pisca led
    - #2*: mede e grava valor
    - #3*: medição mode_autoomática ON
    - #4*: medição mode_autoomática OFF */

  /* O bloco de switch é responsável por alterar as flags relacionadas às teclas de interesse */
  switch (key) {

    case '#':
      flag_hashtag = 1;
      break;

    /* Para os casos 1 a 4, a flag_number só será alterada se '#' já tiver sido apertada no teclado */
    case '1':
      if (flag_hashtag == 1) {
        flag_number = 1;
      }
      break;

    case '2':
      if (flag_hashtag == 1) {
        flag_number = 2;
      }
      break;

    case '3':
      if (flag_hashtag == 1) {
        flag_number = 3;
      }
      break;

    case '4':
      if (flag_hashtag == 1) {
        flag_number = 4;
      }
      break;

    /* A flag_star só recebe 1 se algum número já tiver sido apertado imediatamente depois da hashtag */
    case '*':
      if (flag_number != 0) {
        flag_star = 1;
      }
      else
        flag_hashtag = 0;
      break;
  }

  /* Os blocos de if executam às ações correspondentes aos comandos '#n*', onde 1 <= n <= 4 */
  if (flag_hashtag == 1) {

    if ((flag_number == 1) && (flag_star == 1)) {

      //Pisca o LED indicando que o sistema está responsivo
      digitalWrite(ledPin, HIGH);
      delay(500);
      digitalWrite(ledPin, LOW);
      delay(500);

      flag_number = 0;
      flag_hashtag = 0;
      flag_star = 0;

    }

    if ((flag_number == 2) && (flag_star == 1)) {

      valueLDR = analogRead(A0) / 4;   //converte o valor do LDR para 1 byte
      filesystem_write(valueLDR);

      flag_number = 0;
      flag_hashtag = 0;
      flag_star = 0;
    }

    if ((flag_number == 3) && (flag_star == 1)) {
      mode_auto = 1;            // vai para a função check_modeauto()
      flag_hashtag = 0;
      flag_number = 0;
      flag_star = 0;
    }

    if ((flag_number == 4) && (flag_star == 1)) {
      mode_auto = 0;          
      flag_hashtag = 0;
      flag_number = 0;
      flag_star = 0;
    }
  }

  /* o valor da variável measure é controlada pela função check_modeauto() e realiza as medições automaticamente */
  if (measure == 1) {
    valueLDR = analogRead(A0) / 4;
    filesystem_write(valueLDR);
    Serial.println(valueLDR);
    measure = 0;
  }

  /* Fim das funções do teclado */

  /* A flag_check_command permite separar a recepcao de caracteres
      (vinculada a interrupca) da interpretacao de caracteres. Dessa forma,
      mantemos a rotina de interrupcao mais enxuta, enquanto o processo de
      interpretacao de comandos - mais lento - nao impede a recepcao de
      outros caracteres. Como o processo nao 'prende' a maquina, ele e chamado
      de nao-preemptivo.
  */

  if (flag_check_command == 1) {
    if (str_cmp(Buffer.data, "PING", 4) ) {     //Buffer.data recebe o que é digitado no computador
      sprintf(out_buffer, "PONG\n");
      flag_write = 1;
    }

    if (str_cmp(Buffer.data, "ID", 2) ) {
      sprintf(out_buffer, "ISA E JULIA\n");
      flag_write = 1;
    }

    if (str_cmp(Buffer.data, "MEASURE", 7) ) {
      valueLDR = analogRead(A0) / 4;
      sprintf(out_buffer, "%d\n", valueLDR);
      flag_write = 1;
    }

    if (str_cmp(Buffer.data, "MEMSTATUS", 9) ) {
      sprintf(out_buffer, "%d\n", EEPROM_read(0));
      flag_write = 1;
    }

    if (str_cmp(Buffer.data, "RESET", 5)) {
      EEPROM_write(0, 0);
      buffer_clean();
    }

    if (str_cmp(Buffer.data, "RECORD", 5)) {
      valueLDR = analogRead(A0) / 4;
      filesystem_write(valueLDR);
      flag_write = 1;
    }

    if (str_cmp(Buffer.data, "GET", 3)) {
      int x;
      sscanf(Buffer.data, "%*s %d", &x);
      sprintf(out_buffer, "EEPROM value[%d] = %d\n", x, EEPROM_read(x));
      flag_write = 1;
    }

    flag_check_command = 0;

  }

  /* Posso construir uma dessas estruturas if(flag) para cada funcionalidade
      do sistema. Nesta a seguir, flag_write e habilitada sempre que alguma outra
      funcionalidade criou uma requisicao por escrever o conteudo do buffer na
      saida UART.
  */

  if (flag_write == 1) {
    Serial.write(out_buffer);
    buffer_clean();
    flag_write = 0;
  }

}
