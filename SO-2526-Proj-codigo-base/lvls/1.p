PASSO 1
# POS: comando de colocação inicial do monstro (linha e coluna).
# Assume-se que não é possível o monstro ser colocado
# numa posição impossível/inexistente.
# Só existe um no início por ficheiro.
POS 2 1
# Todos os comandos após PASSO e POS são executados em ciclo infinito.
# Os comandos possíveis são A (esq.), D (dir.), W (cima.), S (baixo)
# R (direcção aleatória), T (espera um número de jogadas), C (carregar)
T 100
S
T 100
S
T 100
W
