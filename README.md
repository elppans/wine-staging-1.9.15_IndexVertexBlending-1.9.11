# wine-staging-1.9.15_IndexVertexBlending-1.9.11-source
Wine 1.9.15 + Staging + path Index Vertex Blending para 1.9.11 sources anexado

Todo o pacote já foi preparado, para a instalação faça:

./configure {parâmetros}

./configure {--help} para saber quais parâmetros usar

make depend
make install -j{2-4}

Não é necessário fazer "make", o "make install" irá fazer toda a configuração e instalar.
-j{2-4} = Opcional, indica quantos cores usar (dependendo do processador). ajuda a compilar mais rápido.
Para mais veja o man do "make".

Para as variáveis consulte o man do "gcc".
