from matplotlib import pyplot as plt
import numpy as np

a = np.zeros( (16, 16) )
with open("q/reader") as f:
    for line in f.readlines():
        parts = line.split(",")
        c1 = int(parts[0])
        c2 = int(parts[1])
        mean = float(parts[2])
        a[(c1, c2)] = mean

for i in range(16):
    for j in range(16):
        if i == j:
            a[ (i,j) ] = np.max(a)

plt.imshow(a, cmap='magma_r')
plt.colorbar(orientation='horizontal', fraction=0.07, anchor=(1.0, 0.0))
plt.savefig('q/readerplot.png')
plt.clf()

a = np.zeros( (16, 16) )
with open("q/writer") as f:
    for line in f.readlines():
        parts = line.split(",")
        c1 = int(parts[0])
        c2 = int(parts[1])
        mean = float(parts[2])
        a[(c1, c2)] = mean

for i in range(16):
    for j in range(16):
        if i == j:
            a[ (i,j) ] = np.max(a)

plt.imshow(a, cmap='magma_r')
plt.colorbar(orientation='horizontal', fraction=0.07, anchor=(1.0, 0.0))
plt.savefig('q/writerplot.png')
