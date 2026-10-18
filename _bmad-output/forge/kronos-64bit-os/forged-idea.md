# Workstation ARM64 própria (nasceu de "Kronos 64-bit")

> Atualizado após a revisão das pendências do spec (`_bmad-output/specs/spec-arm64-stage-workstation/`). O contrato é o spec. Este brief é o resumo.

## Decisões
- Produto próprio, comercial e pronto para escalar. Substitui a ideia de portar o SO do Kronos.
- Diferencial: motor de samples com biblioteca no NVMe muito maior que a RAM (heads em RAM, streaming, lossless). Vem também com interface de áudio USB, que a Nord não tem.
- Software: motor em userspace 64 bits sobre Linux mainline com PREEMPT_RT, sem RTAI. Base única para a v1 e a Pro; sequenciador como módulo; UI num processo isolado do áudio.
- Metas da v1: piso de 256 vozes estéreo de sample, ≤5 ms, até 1 TB indexado, 16 GB de RAM, até 32 bits float/96 kHz, zero dropouts em 24 h, boot em ≤10 s.
- Polifonia: pool dinâmico com pisos (≥256 sample, ≥128 FM, ≥64 VA, órgão sempre com polifonia total). O voice stealing protege notas graves e sustentadas.
- v1 = instrumento de palco com 8 partes, sem sequenciador. Pro = sequenciador, 16 partes, 32 GB.
- A v1 obrigatoriamente tem samples, órgão (com Leslie e multi-contato) e efeitos (rotary, reverb, delay, chorus/phaser, amp/drive de EP, EQ, compressor). FM e VA chegam por firmware.
- Motores clean-room, sem código GPL. sfizz (BSD-2) e libFLAC (BSD) são permitidos. Nada de marcas de terceiros.
- Conteúdo: biblioteca de fábrica fornecida pelo fundador. Importação livre de SFZ (prioridade) e SF2, com subconjunto de opcodes definido.
- Formato: blocos indexados, lossless no estilo FLAC, heads em PCM na RAM. Cripto: AES-CTR por bloco, chave por biblioteca embrulhada por chave do aparelho (OTP/OP-TEE), assinatura Ed25519 e secure boot. O objetivo é dificultar a quebra, não ser inquebrável.
- Transferência: importação por USB-A a partir de SSD externo, mais app de desktop por protocolo controlado. NVMe lacrado e sem mass storage pela USB-C.
- Hardware: SoM industrial RK3588 (16/32 GB) com longevidade em contrato. Plano B: CM5 ou Intel N-series.
- Keybed: Fatar 73SW (semi-pesado) do estoque de ~100 unidades do parceiro, com MCU de varredura dedicado (<1 ms).
- Interface: um knob por função, 9 drawbars físicas e tela touch de 7" para busca, categorias e favoritos.
- Áudio: I2S/TDM → PCM3168A como clock master. USB-C como interface UAC2 + MIDI.
- I/O: 4 saídas balanceadas, fone, 1 entrada estéreo, USB-C, USB-A, MIDI DIN e pedais. Sem Bluetooth e sem Wi-Fi.
- Robustez: sistema somente leitura, firmware A/B com rollback e importações atômicas.
- Mercado: lança primeiro no Brasil. v1 a R$ 17.990–19.990 (concorre com Electro 6D e YC73 pelo valor). Pro a partir de R$ 24.990.
- Canal: as ~100 primeiras unidades vendidas direto, lojas depois.
- Certificação: sem Anatel (sem rádio), fonte em conformidade com o Inmetro, testes pela IEC 62368-1 para exportar depois.
- Aceitação: null tests (samples, FM) e ABX cego do órgão, usando o Hammond M-solo como referência por MIDI.
- Datas: Marco 1 no mês 6, primeiras unidades tocáveis no mês 12, lançamento da v1 no mês 18.

## Rejeitados
- Portar ou estender o SO do Kronos: o OA.ko é um binário i386 fechado, com teto no espaço virtual do kernel.
- Pitch "Kronos 4" para a Korg.
- i.MX 95/8M Plus, CIX P1 e CM5 como produto.
- USB interno (XMOS) no caminho de áudio, NVMe acessível ao usuário, rádio na v1.
- Preço abaixo de R$ 15 mil: não cabe com impostos e margem em baixo volume.

## Pontos ainda frágeis
- PREEMPT_RT e painel DSI no mainline do RK3588 ainda sem medição.
- Software inteiro com uma pessoa e IA.
- Margem real depende dos impostos sobre as peças (falta validar com um contador).
- Recompra de keybeds Fatar depois das 100 unidades.
