"""Envia pacotes de teste para a FRDM-KL25Z.  Uso: python3 teste.py [/dev/ttyACM0]"""
import sys, time, serial

PORTA = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
ID = 0xA5
s = serial.Serial(PORTA, 115200, timeout=1)

def pacote(r, g, b, acao, id_=ID, check_errado=False):
    corpo = [id_, r, g, b, acao]
    c = 0
    for x in corpo:
        c ^= x
    if check_errado:
        c ^= 0xFF
    return bytes(corpo + [c])

def enviar(nome, dados, pausa=2):
    print(f"{nome:<28} -> {dados.hex(' ')}")
    s.write(dados)
    time.sleep(pausa)

enviar("liga vermelho",        pacote(255, 0, 0, 0x01))
enviar("liga azul",            pacote(0, 0, 255, 0x01))
enviar("toggle (apaga)",       pacote(0, 0, 255, 0x03))
enviar("toggle (acende)",      pacote(0, 0, 255, 0x03))
enviar("pisca verde 1s",       pacote(0, 255, 0, 0x04), pausa=5)
enviar("pisca amarelo 2s",     pacote(255, 255, 0, 0x05), pausa=7)
enviar("desliga",              pacote(0, 0, 0, 0x02))

print("--- casos invalidos: o LED NAO deve mudar ---")
enviar("ID errado",            pacote(255, 0, 0, 0x01, id_=0x11))
enviar("CHECK errado",         pacote(255, 0, 0, 0x01, check_errado=True))
enviar("acao invalida (0x09)", pacote(255, 0, 0, 0x09))
enviar("truncado (3 bytes)",   pacote(255, 0, 0, 0x01)[:3], pausa=0.5)
enviar("liga branco (valido)", pacote(255, 255, 255, 0x01))  # prova que se recuperou
s.close()