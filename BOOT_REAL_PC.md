## Вимоги до заліза

| Компонент | Вимоги                      |
|-----------|-----------------------------|
| CPU       | x86 32-bit					        |
| RAM       | Мінімум 32 МБ				        |
| Display   | будь-який				            |
| BIOS      | Legacy BIOS або UEFI		    |
| Диск      | По можливості(працює з USB) |

## UEFI vs Legacy BIOS

MyOS використовує GRUB котрий підтримує оба режимиа:
- **Legacy BIOS**: працює з коробки
- **UEFI**: потрібен CSM (Compatibility Support Module) в налаштуваннях BIOS
  - В BIOS знайти: `Boot Mode = Legacy` або `CSM = Enabled`
  - Secure Boot повинен бути вимкненним


```
VirtualBox:
  New VM → Type: Other → Version: Other/Unknown (32-bit)
  RAM: 128+ МБ
  Storage: выбери myos.iso как оптический диск

VMware Player:
  Create New VM → I will install... → Other → next
  Disk: use ISO → myos.iso
```

