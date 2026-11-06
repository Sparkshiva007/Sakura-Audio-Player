#ifndef SD_SPI_H_
#define SD_SPI_H_

#include <stdint.h>
#include <stdbool.h>

int sd_spi_hw_init(void);
int sd_spi_card_init(void);
bool sd_spi_is_card_inserted(void);
bool sd_spi_is_ready(void);
uint32_t sd_spi_get_sector_count(void);
int sd_spi_read_sector(uint32_t sector, uint8_t *buf);
int sd_spi_read_sectors(uint32_t start_sector, uint8_t *buf, uint32_t count);
int sd_spi_write_sector(uint32_t sector, const uint8_t *buf);
int sd_spi_write_sectors(uint32_t start_sector, const uint8_t *buf, uint32_t count);

#endif /* SD_SPI_H_ */
