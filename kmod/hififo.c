/* 
 * HIFIFO: Harmon Instruments PCI Express to FIFO
 * Copyright (C) 2014 Harmon Instruments, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <linux/sysfs.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/cdev.h>
#include <linux/ioctl.h>

#define VENDOR_ID 0x10EE
#define DEVICE_ID 0x7024
#define DEVICE_NAME "hififo"

#define HIFIFO_IOC_MAGIC 'f'
#define IOC_INFO 0x10
#define IOC_GET_TO_PC 0x11
#define IOC_PUT_TO_PC 0x12
#define IOC_GET_FROM_PC 0x13
#define IOC_PUT_FROM_PC 0x14
#define IOC_SET_TIMEOUT 0x15

#define MAX_FIFOS 4

static struct pci_device_id hififo_pci_table[] = {
  {VENDOR_ID, DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID, 0, 0 ,0},
  {0,}
};

#define BUFFER_PAGES_FROM_PC 4
#define BUFFER_PAGES_TO_PC 2
#define BUFFER_MAX_PAGES 32
#define BUFFER_PAGE_SIZE (2048*1024) // this is the size of a page in the card's page table
#define BUFFER_SIZE_TO_PC (BUFFER_PAGE_SIZE*BUFFER_PAGES_TO_PC)
#define BUFFER_SIZE_FROM_PC (BUFFER_PAGE_SIZE*BUFFER_PAGES_FROM_PC)

#define REG_INTERRUPT 0
#define REG_TO_PC_COUNT 2
#define REG_TO_PC_STOP 3
#define REG_TO_PC_MATCH 4
#define REG_FROM_PC_COUNT 5
#define REG_FROM_PC_STOP 6
#define REG_FROM_PC_MATCH 7
#define REG_RESET 8
#define REG_TO_PC_PAGE_TABLE_BASE 32
#define REG_FROM_PC_PAGE_TABLE_BASE 128

static struct class *hififo_class;

struct hififo_fifo1 {
  dma_addr_t dma_addr[BUFFER_MAX_PAGES];
  u64 *buffer[BUFFER_MAX_PAGES];
  u64 buffer_size;
  u64 pointer;
  wait_queue_head_t queue;
  int enabled;
};

struct hififo_fifo {
  struct hififo_fifo1 to_pc;
  struct hififo_fifo1 from_pc;
  u64 *pio_reg_base;
  struct cdev cdev;
  int is_open;
};

struct hififo_dev {
  struct hififo_fifo * fifo[MAX_FIFOS];
  u64 *pio_reg_base;
  int major;
  int nfifos;
};

#define fifo_writereg(data, addr) (writeq(cpu_to_le64(data), &fifo->pio_reg_base[addr]))
#define fifo_readreg(addr) le32_to_cpu(readl(&fifo->pio_reg_base[addr]))

#define global_writereg(data, addr) (writeq(cpu_to_le64(data), &drvdata->pio_reg_base[addr]))
#define global_readreg(addr) le32_to_cpu(readl(&drvdata->pio_reg_base[addr]))

static int hififo_open(struct inode *inode, struct file *filp){
  struct hififo_fifo *fifo = container_of(inode->i_cdev, struct hififo_fifo, cdev);
  filp->private_data = fifo;
  printk("hififo: open\n");
  if (fifo->is_open)
    return -EBUSY;
  fifo->is_open++;
  try_module_get(THIS_MODULE); // increment the usage count
  return 0;
}

static int hififo_release(struct inode *inode, struct file *filp){
  struct hififo_fifo *fifo = filp->private_data;
  fifo->is_open--;
  module_put(THIS_MODULE); // decrement the usage count
  printk("hififo: close\n");
  return 0;
}

static u64 get_bytes_in_ring_to_pc(struct hififo_fifo *fifo){
  u64 bytes_in_buffer = (fifo_readreg(REG_TO_PC_COUNT)- fifo->to_pc.pointer) & (BUFFER_SIZE_TO_PC - 1);
  return bytes_in_buffer;
}

static u64 wait_bytes_in_ring_to_pc(struct hififo_fifo *fifo, u64 desired_bytes){
  u64 bytes_in_buffer = get_bytes_in_ring_to_pc(fifo);
  int retval;
  if(bytes_in_buffer >= desired_bytes)
    return bytes_in_buffer;
  fifo_writereg(fifo->to_pc.pointer + desired_bytes, REG_TO_PC_MATCH);
  bytes_in_buffer = get_bytes_in_ring_to_pc(fifo);
  if(bytes_in_buffer >= desired_bytes)
    return bytes_in_buffer;
  retval = wait_event_interruptible_timeout(fifo->to_pc.queue, (get_bytes_in_ring_to_pc(fifo) >= desired_bytes), HZ/4); // retval: 0 if timed out, else number of jiffies remaining
  bytes_in_buffer = get_bytes_in_ring_to_pc(fifo);
  return bytes_in_buffer;
}

static u64 get_bytes_in_ring_from_pc(struct hififo_fifo *fifo){
  u64 bytes_available = fifo_readreg(REG_FROM_PC_COUNT);
  while((bytes_available & 0x03) != 0){
    bytes_available = fifo_readreg(REG_FROM_PC_COUNT);
    printk("retry: REG_FROM_PC_COUNT = %llx\n", bytes_available);
  }
  bytes_available = (fifo->from_pc.pointer - bytes_available);
  bytes_available &= (BUFFER_SIZE_FROM_PC - 1);
  bytes_available = (BUFFER_SIZE_FROM_PC - 1024) - bytes_available;
  return bytes_available;
}

static u64 wait_bytes_in_ring_from_pc(struct hififo_fifo *fifo, u64 desired_bytes){
  u64 bytes_available = get_bytes_in_ring_from_pc(fifo);
  int retval;
  if(bytes_available >= desired_bytes)
    return bytes_available;

  fifo_writereg((fifo->from_pc.pointer - (BUFFER_SIZE_FROM_PC - desired_bytes)), REG_FROM_PC_MATCH);
  bytes_available = get_bytes_in_ring_from_pc(fifo);
  if(bytes_available >= desired_bytes)
    return bytes_available;
  retval = wait_event_interruptible_timeout(fifo->from_pc.queue, (get_bytes_in_ring_from_pc(fifo) >= desired_bytes), HZ/4); // retval: 0 if timed out, else number of jiffies remaining
  bytes_available = get_bytes_in_ring_from_pc(fifo);
  return bytes_available;
}

static ssize_t hififo_read(struct file *filp,
			    char *buffer,
			    size_t length,
			    loff_t * offset){
  //struct hififo_fifo *fifo = filp->private_data;
  return -EINVAL;
}

static ssize_t hififo_write(struct file *filp,
			     const char *buf,
			     size_t length,
			     loff_t * off){
  //struct hififo_fifo *fifo = filp->private_data;
  return -EINVAL;
}

static long hififo_ioctl (struct file *file, unsigned int command, unsigned long arg){
  struct hififo_fifo *fifo = file->private_data;
  u64 tmp[8];
  switch(command) {
    // get info
  case _IOR(HIFIFO_IOC_MAGIC, IOC_INFO ,u64[8]):
    tmp[0] = BUFFER_SIZE_TO_PC;
    tmp[1] = BUFFER_SIZE_FROM_PC;
    tmp[2] = fifo->to_pc.pointer & (BUFFER_SIZE_TO_PC - 1);
    tmp[3] = fifo->from_pc.pointer & (BUFFER_SIZE_FROM_PC - 1);
    tmp[4] = 0;
    tmp[5] = 0;
    tmp[6] = 0;
    tmp[7] = 0;
    if(copy_to_user((void *) arg, tmp, 8*sizeof(u64)) != 0)
      return -EFAULT;
    return 0;
    // get bytes available in to PC FIFO
  case _IO(HIFIFO_IOC_MAGIC, IOC_GET_TO_PC):
    return wait_bytes_in_ring_to_pc(fifo, arg);
    // accept bytes from to PC FIFO
  case _IO(HIFIFO_IOC_MAGIC, IOC_PUT_TO_PC):
    fifo->to_pc.pointer += arg;
    fifo_writereg(fifo->to_pc.pointer + (BUFFER_SIZE_TO_PC - 128), REG_TO_PC_MATCH);
    fifo_writereg(fifo->to_pc.pointer + (BUFFER_SIZE_TO_PC - 128), REG_TO_PC_STOP);
    return 0;
    // get bytes available in from PC FIFO
  case _IO(HIFIFO_IOC_MAGIC, IOC_GET_FROM_PC):
    return wait_bytes_in_ring_from_pc(fifo, arg);
    // commit bytes to from PC FIFO
  case _IO(HIFIFO_IOC_MAGIC, IOC_PUT_FROM_PC):
    fifo->from_pc.pointer += arg;
    fifo_writereg(fifo->from_pc.pointer, REG_FROM_PC_MATCH);
    fifo_writereg(fifo->from_pc.pointer, REG_FROM_PC_STOP);
    return 0;
  }
  return -ENOTTY;
}

static int hififo_mmap(struct file *file, struct vm_area_struct *vma)
{
  struct hififo_fifo *fifo = file->private_data;
  unsigned long size;
  int retval, i, j;
  off_t uaddr = 0;

  if (!(vma->vm_flags & VM_SHARED))
    return -EINVAL;
  if (vma->vm_start & ~PAGE_MASK)
    return -EINVAL;

  size = vma->vm_end - vma->vm_start;

  if (size != 2 * BUFFER_PAGE_SIZE * (BUFFER_PAGES_TO_PC + BUFFER_PAGES_FROM_PC))
    return -EINVAL;

  /* to PC */
  for(i=0; i<2; i++){
    for(j=0; j<BUFFER_PAGES_TO_PC; j++){
      retval = remap_pfn_range(vma,
			       vma->vm_start + uaddr, 
			       fifo->to_pc.dma_addr[j] >> PAGE_SHIFT,
			       BUFFER_PAGE_SIZE,
			       vma->vm_page_prot);
      if(retval)
	return retval;
      uaddr += BUFFER_PAGE_SIZE;
    }
  }
  /* from PC */
  for(i=0; i<2; i++){
    for(j=0; j<BUFFER_PAGES_FROM_PC; j++){
      retval = remap_pfn_range(vma,
			       vma->vm_start + uaddr, 
			       fifo->from_pc.dma_addr[j] >> PAGE_SHIFT,
			       BUFFER_PAGE_SIZE,
			       vma->vm_page_prot);
      if(retval)
	return retval;
      uaddr += BUFFER_PAGE_SIZE;
    }
  }
  return 0;
}

static struct file_operations fops = {
 .read = hififo_read,
 .write = hififo_write,
 .unlocked_ioctl = hififo_ioctl,
 .mmap = hififo_mmap,
 .open = hififo_open,
 .release = hififo_release
};

static irqreturn_t hififo_interrupt(int irq, void *dev_id, struct pt_regs *regs){
  struct hififo_dev *drvdata = dev_id;
  u64 sr = global_readreg(REG_INTERRUPT);
  if((sr & 0x000F) && (drvdata->fifo[0] != NULL)){
    if(sr & 0x0003)
      wake_up_all(&drvdata->fifo[0]->to_pc.queue);
    if(sr & 0x000C)
      wake_up_all(&drvdata->fifo[0]->from_pc.queue);
  }
  printk("hififo interrupt: sr = %llx\n", sr);
  return IRQ_HANDLED;
}

static int hififo_alloc_buffer(struct pci_dev *pdev, struct hififo_fifo1 * fifo, int pages){
  int i, j;
  for(i=0; i<pages; i++){
    fifo->buffer[i] = pci_alloc_consistent(pdev, BUFFER_PAGE_SIZE, &fifo->dma_addr[i]);
    if(fifo->buffer[i] == NULL){
      printk("Failed to allocate DMA buffer %d\n", i);
      return -ENOMEM;
    }
    else{
      for(j=0; j<BUFFER_PAGE_SIZE/8; j++)
	fifo->buffer[i][j] = 0;
    }
  }
  return 0;
}

static int hififo_probe(struct pci_dev *pdev, const struct pci_device_id *id){
  int i;
  int retval;
  dev_t dev = 0;
  char tmpstr[16];
  struct hififo_dev *drvdata;

  drvdata = devm_kzalloc(&pdev->dev, sizeof(struct hififo_dev), GFP_KERNEL);
  if (!drvdata){
    printk(KERN_ERR DEVICE_NAME "failed to alloc drvdata\n");
    return -ENOMEM;
  }
    
  retval = pcim_enable_device(pdev);
  if(retval){
    printk(KERN_ERR DEVICE_NAME ": pcim_enable_device() failed\n");
    return retval;
  }

  retval = pci_request_regions(pdev, DEVICE_NAME);
  if(retval < 0){
    printk(KERN_ERR DEVICE_NAME ": pci_request_regions() failed\n");
    return retval;
  }

  printk(KERN_INFO DEVICE_NAME ": Found Harmon Instruments PCI Express interface board\n");
  pci_set_drvdata(pdev, drvdata);

  pci_set_master(pdev); /* returns void */

  pci_set_dma_mask(pdev, 0xFFFFFFFFFFFFFFFF);

  pci_set_consistent_dma_mask(pdev, 0xFFFFFFFFFFFFFFFF);

  retval = pci_enable_msi(pdev);
  if(retval < 0){
    printk(KERN_ERR DEVICE_NAME ": pci_enable_msi() failed\n");
    return retval;
  }

  retval = devm_request_irq(&pdev->dev, pdev->irq, (irq_handler_t) hififo_interrupt, 0 /* flags */, DEVICE_NAME, drvdata);
  if(retval){
    printk(KERN_ERR DEVICE_NAME ": request_irq() failed\n");
    return retval;
  }

  drvdata->pio_reg_base = (u64 *) pcim_iomap(pdev, 0, 65536);
  printk(KERN_INFO DEVICE_NAME ": pci_resource_start(dev, 0) = 0x%.8llx, virt = 0x%.16llx\n", (u64) pci_resource_start(pdev, 0), (u64) drvdata->pio_reg_base);

  drvdata->nfifos = 1;

  retval = alloc_chrdev_region(&dev, 0, drvdata->nfifos, DEVICE_NAME);
  if (retval) {
    printk(KERN_ERR DEVICE_NAME ": alloc_chrdev_region() failed\n");
    return retval;
  }

  drvdata->major = MAJOR(dev);
  
  for(i=0; i<drvdata->nfifos; i++){
    drvdata->fifo[i] = devm_kzalloc(&pdev->dev, sizeof(struct hififo_fifo), GFP_KERNEL);
    if (!drvdata->fifo[i]){
      printk(KERN_ERR DEVICE_NAME "failed to alloc hififo_fifo\n");
      return -ENOMEM;
    }
    cdev_init(&drvdata->fifo[i]->cdev, &fops); // void
    drvdata->fifo[i]->cdev.owner = THIS_MODULE;
    drvdata->fifo[i]->cdev.ops = &fops;

    retval = cdev_add (&drvdata->fifo[i]->cdev, MKDEV(MAJOR(dev), i), 1);
    if (retval){
      printk(KERN_NOTICE DEVICE_NAME ": Error %d adding cdev\n", retval);
      return retval;
    }  
    sprintf(tmpstr, "hififo%d", i);
    device_create(hififo_class, NULL, MKDEV(MAJOR(dev), i), NULL, tmpstr);
    drvdata->fifo[i]->to_pc.enabled = 1;
    drvdata->fifo[i]->from_pc.enabled = 1;
    drvdata->fifo[i]->pio_reg_base = drvdata->pio_reg_base;
    init_waitqueue_head(&drvdata->fifo[i]->to_pc.queue);
    init_waitqueue_head(&drvdata->fifo[i]->from_pc.queue);
    if(hififo_alloc_buffer(pdev, &drvdata->fifo[i]->from_pc, BUFFER_PAGES_FROM_PC) != 0)
      return -ENOMEM;
    if(hififo_alloc_buffer(pdev, &drvdata->fifo[i]->to_pc, BUFFER_PAGES_TO_PC) != 0)
      return -ENOMEM;
  }

  for(i=0; i<8; i++)
    printk("bar0[%d] = %.8x\n", i, (u32) global_readreg(i));

  /* disable it */
  global_writereg(0xF, REG_RESET);
  udelay(10); /* wait for completion of anything that was running */
  /* set interrupt match registers */
  global_writereg(0, REG_TO_PC_MATCH);
  global_writereg(0, REG_FROM_PC_MATCH); 
  /* enable interrupts */
  global_writereg(0xF, REG_INTERRUPT);
  /* load the card page tables */
  for(i=0; i<32; i++){
    global_writereg(drvdata->fifo[0]->to_pc.dma_addr[i%BUFFER_PAGES_TO_PC], i+REG_TO_PC_PAGE_TABLE_BASE);
    global_writereg(drvdata->fifo[0]->from_pc.dma_addr[i%BUFFER_PAGES_FROM_PC], i+REG_FROM_PC_PAGE_TABLE_BASE);
  }
  for(i=0; i<8; i++)
    printk("bar0[%d] = %.8x\n", i, global_readreg(i));
  /* enable it */
  global_writereg(0, REG_RESET);
  global_writereg(BUFFER_SIZE_TO_PC-1024, REG_TO_PC_STOP);
  drvdata->fifo[0]->to_pc.pointer = 0;
  return 0;
}

static void hififo_remove(struct pci_dev *pdev){
  struct hififo_dev *drvdata = pci_get_drvdata(pdev);
  int i;
  global_writereg(0xF, REG_RESET);
  for(i=0; i<drvdata->nfifos; i++)
    device_destroy(hififo_class, MKDEV(drvdata->major, i));
  unregister_chrdev_region (MKDEV(drvdata->major, 0), drvdata->nfifos);
}

static struct pci_driver hififo_driver = {
  .name = "hififo",
  .id_table = hififo_pci_table,
  .probe = hififo_probe,
  .remove = hififo_remove,
  // see Documentation/PCI/pci.txt - pci_register_driver call
};

static int __init hififo_init(void){
  printk ("Loading hififo kernel module\n");
  hififo_class = class_create(THIS_MODULE, "hififo");
  if (IS_ERR(hififo_class)) {
    printk(KERN_ERR "Error creating hififo class.\n");
    //goto error;
  }
  return pci_register_driver(&hififo_driver);
}

static void __exit hififo_exit(void){
  pci_unregister_driver(&hififo_driver);
  class_destroy(hififo_class);
  printk ("Unloading hififo kernel module.\n");
  return;
}

module_init(hififo_init);
module_exit(hififo_exit);

MODULE_LICENSE("GPL");
