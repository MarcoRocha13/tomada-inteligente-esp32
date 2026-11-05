# Tomada inteligente com monitoramento e controle remoto de energia

Protótipo desenvolvido como Trabalho de Conclusão de Curso (Engenharia Elétrica, Centro Universitário Campo Real, 2026).
Um **ESP32** lê um medidor **PZEM-004T v3**, aciona um contator por um módulo relé e se comunica por **MQTT**.
Um fluxo **Node-RED** (em Docker) grava os dados em **SQLite**, mostra um painel, calcula o custo da energia e conversa por **WhatsApp** (Evolution API).

> Este repositório **não contém** senhas, chaves nem endereços de acesso. Você precisa informar os seus (veja "Configuração").
> O protótipo é acadêmico e foi ensaiado em bancada com carga resistiva. **Não é um produto certificado.** Instalações elétricas devem seguir a NBR 5410.

## Estrutura

```
firmware/tomada_inteligente/
  tomada_inteligente.ino   firmware do ESP32 (Arduino)
  secrets.example.h        modelo do arquivo de credenciais (copie para secrets.h)
node-red/
  flows.json               fluxo do Node-RED (painel, banco, alertas, WhatsApp)
  variaveis-de-ambiente.exemplo   modelo das variáveis de ambiente do Node-RED
```

## Funcionamento resumido

- Leitura do PZEM a cada 500 ms e publicação da telemetria a cada 10 s (`casa/quadrogeral/...`).
- Seccionamento local: corrente acima de 20 A (imediato) ou potência/corrente acima do valor configurado em 3 leituras seguidas.
- Tensão fora de 117 V a 133 V apenas é sinalizada no painel.
- A carga sempre inicia desligada. Comandos pelo painel, WhatsApp e temporizador, com a origem registrada.

## Configuração

### 1. Firmware (ESP32)

1. Instale no Arduino IDE o suporte à placa ESP32 e as bibliotecas: `PZEM004Tv30`, `PubSubClient`, `WiFiManager`, `Adafruit GFX Library` e `Adafruit SSD1306`.
2. Na pasta do `.ino`, copie `secrets.example.h` para `secrets.h` e preencha o host, a porta, o usuário e a senha do seu broker MQTT (por exemplo, um cluster gratuito do HiveMQ Cloud, com TLS na porta 8883).
3. Compile e grave. Na primeira energização o ESP32 abre o ponto de acesso `TomadaInteligente-Config` para você informar o Wi-Fi e a potência do equipamento.

Pinos: relé GPIO26, LED verde GPIO25, LED vermelho GPIO33, PZEM em UART2 (RX GPIO16, TX GPIO17), display OLED I²C (SDA GPIO21, SCL GPIO22) e botão de reset de configuração no GPIO0.

### 2. Node-RED

1. Instale os pacotes `@flowfuse/node-red-dashboard` (Dashboard 2.0) e `node-red-node-sqlite`.
2. Importe `node-red/flows.json`.
3. Defina as variáveis de ambiente do Node-RED (modelo em `node-red/variaveis-de-ambiente.exemplo`):

| Variável | Para que serve |
|---|---|
| `MQTT_HOST` | host do broker MQTT |
| `EVOLUTION_API_KEY` | chave da Evolution API |
| `WHATSAPP_GRUPO` | identificador do grupo do WhatsApp que recebe e envia os comandos (termina em `@g.us`) |
| `EVOLUTION_URL_ENVIO` | URL completa de envio, por exemplo `http://host.docker.internal:8081/message/sendText/NOME_DA_INSTANCIA` |
| `EVOLUTION_URL_ESTADO` | URL completa do estado da instância, por exemplo `http://host.docker.internal:8081/instance/connectionState/NOME_DA_INSTANCIA` |

   Com Docker Compose, use `env_file` (e mantenha esse arquivo fora do repositório) ou `environment`, e recrie o contêiner:

   ```yaml
   services:
     nodered:
       image: nodered/node-red:latest
       env_file: ./node-red.env      # não publique este arquivo
       ports: ["1880:1880"]
       volumes: ["nodered_data:/data"]
   ```

4. No nó do broker `TCC_ENERGIA`, abra a aba **Segurança** e informe o usuário e a senha do MQTT. Eles ficam no armazenamento de credenciais do Node-RED, e não neste arquivo.
5. Na Evolution API (v2), configure o webhook da sua instância para `http://host.docker.internal:1880/whatsapp` com o evento `MESSAGES_UPSERT`.

## Avisos de segurança

- No firmware, a conexão TLS usa `setInsecure()` (não valida o certificado do servidor). Para uso real, valide o certificado.
- As rotas `/exportar/...` e o painel do Node-RED não têm autenticação. Exponha-os apenas em rede privada (por exemplo, Tailscale).
- Nunca publique `secrets.h`, o arquivo de variáveis de ambiente, `flows_cred.json` nem o banco `.db`.

## Licença

MIT (veja `LICENSE`). Ajuste se preferir outra.
