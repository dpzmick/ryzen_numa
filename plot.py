from matplotlib import pyplot as plt
import numpy as np

a = np.zeros( (16, 16) )
with open("bw_baseline/out") as f:
    for line in f.readlines():
        parts = line.split(",")
        c1 = int(parts[0])
        c2 = int(parts[1])
        mean = float(parts[2])
        a[(c1, c2)] = mean

plt.imshow(a, cmap='magma_r')
plt.colorbar(orientation='horizontal', fraction=0.07, anchor=(1.0, 0.0))
plt.savefig('bw_baseline/plot.png')
#plt.show()
